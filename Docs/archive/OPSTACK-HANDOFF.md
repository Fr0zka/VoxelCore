# VoxelForge handoff — the operator stack is DONE. Read this, not the old jargon.

> Paste this into a fresh session. Written 2026-08-16, deliberately in plain language: the previous
> version of this file had become a private dialect that Jahni could not read, which is a failure of
> the document, not of the reader.

---

## 1. What the operator stack was, in one paragraph

Cave generation used to be one big `switch`: each cave type (tunnels, maze, shafts, floating
islands, surface…) was its own hardcoded ~200–1000 line C++ function. The refactor replaced that with
small composable pieces ("operators") that stack up to produce the same terrain. **The promise was
that you could eventually invent new world types by combining pieces in the editor instead of asking
for another thousand-line function.**

**That promise — "Phase 3", ops as data assets — was never built, and is NOT being built now.**

## 2. Status: done. Stop refactoring.

- **8 of 8 archetypes ported**, running in production (`bUseOperatorStack` is ticked on the game's
  strate assets), and **bit-for-bit identical** to the old path. Verified by a 115 000-sample field
  digest plus eight per-archetype equivalence tests.
- **14/14 automation tests green, 0 violations anywhere.**
- **39 % of tiles are skipped in the running game** (`Tiles Meshed` 1.15 vs `Tiles Classified` 1.88).
  It was 0 % on the morning of 2026-08-16. Each skipped tile avoids ~43 000 density evaluations plus
  marching cubes.

**Decision taken 2026-08-16, with Jahni:** the op stack is finished. **Do not start Phase 3. Do not
start another refactor.** The old `switch` stays in place as the correctness oracle — deleting it
buys nothing today. Next work should be things Jahni can *see*: see `fable-idea.md` (F7 set-pieces,
F9 audio were queued before this started).

### The honest ledger, so nobody re-litigates it

Three weeks, 120 commits, from 2026-07-27. Every `feat:` commit in that window is a *port* of
something that already worked. **The world did not change by a single voxel — that was the
acceptance criterion.** What Jahni actually got: the 39 % perf win, two genuine pre-existing bugs
found (a use-after-free on every strate-asset edit while streaming, and an under-bounded room
collection that could make two multiplayer peers generate different geometry), and a number of fixes
to bugs the refactor itself introduced. That is a thin return for three weeks, and it is why the
direction changed.

## 3. The jargon, translated

Almost all of it means one thing: **can we prove a chunk of world is entirely rock or entirely air
without checking every point in it, so we can skip the expensive work?**

| term | plain meaning |
|---|---|
| **T1.d / tile skipping** | that idea. The single biggest perf item in the plan. |
| **box verdict / `ClassifyBox`** | "is this whole box uniform?" → `AllSolid`, `AllAir`, or `Mixed` (don't know) |
| **`Mixed`** | "can't prove it" — always safe, just means we do the work |
| **`ClassifyTile`** | the function that decides, per tile, whether to skip meshing |
| **operator / op stack** | one generation step (rock, carve, roughness…) and the list of them |
| **equivalence test** | proof the new path produces byte-identical terrain to the old one |
| **`violations`** | ⚠️ **the only number that means danger.** A tile wrongly proved uniform has *no geometry and no collision* — a player falls through the floor. Must always be 0. |

## 4. What is verified, and what is not

**Verified:** everything through commit `871ca19` — tests green, digests unchanged, 39 % measured
in game.

⚠️ **Built but NOT re-verified:** `4d33321` (Sol's boundary fold — lets a tile that straddles cave and
open air still resolve) and `91585ea` (a test-only warning demotion). Jahni built these and says the
game *looks* fine, but **the test suite has not been re-run and the counters have not been re-read
since.** Before trusting them:

1. run the `VoxelForge` automation filter — **`violations` must be 0 and all eight equivalences
   bit-identical**;
2. `stat VoxelForge` in game — the accounting must close:
   `Tiles Meshed + Skipped All Air + Skipped All Solid = Tiles Classified`.

If either fails, `git revert 4d33321` — the 39 % win does not depend on it.

## 5. Rules that still prevent real bugs

- **Never build.** Jahni builds; he has the editor open and it costs him real time. Say "ready to
  build" and list likely compile-error spots.
- **`violations` 0 and the eight equivalences bit-identical** — the only non-negotiable results.
- **A bound used to skip work must be PROVED, not observed.** Use `VF_PerlinAbsBound` (= 1.5);
  `FMath::Lerp(A,B,t)` spans `[min(A,B), max(A,B)]`, so a radius envelope is `max(Min,Max)`.
- **Never change `ClassifyTile`'s conditions or return values casually.** Every `return` there fails
  safe to `Mixed`.
- **Never state a `.uasset` value from memory** (like `bUseOperatorStack`). Ask, or read it in the
  editor. This sent a full day sideways.
- `FindSlotIndexForChunkZ` is **protected**; `GetStrateChunkZBounds` is the public equivalent.
- Push `experimental` freely. **Never push `main`.**

## 6. Two lessons that generalise beyond this plugin

- **A signal that always says the same thing measures nothing.** A counter that can fire for two
  reasons is not a measurement — splitting one such counter is what finally cracked T1.d after a day
  of wrong inference. A warning that fires every run and always means "this is fine" is noise that
  trains the reader to ignore warnings; one of those quietly worried Jahni for several sessions.
- **Verify the premise, and verify it completely.** Multiple confident chains reversed on checking
  this month. Twice the failure was a *partial* read — grepping a symbol and reporting it as checked
  for something else. A grep that finds a declaration has not checked its access specifier.

## 7. The open question, which matters more than any of the above

**What do you want the world to *do* that it doesn't?** Three weeks went into a pipeline instead of
that question. Start there.
