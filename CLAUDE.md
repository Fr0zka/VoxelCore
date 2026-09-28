# CLAUDE.md — VoxelForge

UE5 **density-field voxel terrain** plugin (strates / Marching Cubes / async streaming).
Don't re-read the whole plugin — use the map.

## ⛔ #1 rule: NEVER build or compile
Jahni runs every build himself (he has the editor open; running it via Claude just burns cost).
When code changes are done, **stop and say "ready to build"** + list likely compile-error spots,
then wait for his results / pasted errors. Same for in-editor checks — ask for a screenshot, don't
try to run anything.

## Read first (in order, load only what the task needs)
- **[CODEMAP.md](CODEMAP.md) — ALWAYS.** Navigation: what it is, data-flow (§2), symbol→`file:line`
  index (§3), "I want to change X → go here" (§5), conventions & gotchas (§6). Trust **symbol names
  over line numbers** (lines drift).
- **[ARCHITECTURE.md](ARCHITECTURE.md) — when touching generation / strates / passages / biomes.**
  The deep design (archetypes, (0,0) spine, disturbances, content/atmosphere, biomes) **and the
  `§8.10` performance invariants ("don't regress").**
- **[fable-idea.md](fable-idea.md) — before planning PERF or FEATURE work.** Ranked perf+feature
  roadmap with verified hot spots; check it so you don't re-propose done/known work. (A perf pass
  already shipped: game thread solved ~3.94 ms, CullTiles spiral fixed, region-granular foliage.)
- **[REVIEW_FINDINGS.md](REVIEW_FINDINGS.md)** — open quality/cleanup items (cross-check vs fable-idea
  before acting; some may already be addressed).

## Hard rules (these prevent real bugs — verify against code, don't assume)
- **Density sign: negative = solid, positive = air** (MC convention at the mesher).
  `FVoxelModification::Strength` negative = carve. *The #1 source of confusion.*
- **Density functions take VOXEL coords, not cm.** Conversions live in `VoxelTypes.h`.
- **Determinism:** all randomness = hash of (coord, seed, strateIndex). No RNG state — same seed ⇒
  same world. Player edits (diff layer) are the only non-deterministic overlay.
- **Async safety:** worker tasks only READ Generator/Mesher, must check `bShuttingDown`, and return via
  the result queues — `ProcessQueue` and its `CriticalProcessQueue` / `EditedProcessQueue` /
  `NearProcessQueue` siblings (all must stay `EQueueMode::Mpsc`). `EndPlay` blocks on `ActiveTaskCount → 0`.
- **Carry the `Epoch`** through any new async path (stale results are dropped on mismatch).
- **Don't "optimize" the perf invariants** in ARCHITECTURE `§8.10` — the `thread_local` box-valid
  caches, two-pass MC loop, SSE noise, and clipmap streaming are intentional.
- **Never edit** `Binaries/`, `Intermediate/`, `*.generated.h`. Comments are French + English — match
  the surrounding file.

## Discipline (keep the map alive)
- Add / rename / move a symbol → update its **CODEMAP §3** row (symbol-first; line is a hint).
- Change generation/strate design → update **ARCHITECTURE §8**.
- Resolve a checklist item → tick it in **REVIEW_FINDINGS.md**.
- Don't duplicate content across these files — each has one job (map / design / findings / rules).
