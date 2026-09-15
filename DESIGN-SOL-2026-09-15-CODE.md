# DESIGN-SOL-2026-09-15-CODE: round two, Sol reads the code

Continues `DESIGN-SOL-2026-09-15.md`. At the owner's request, Sol's round-one session was forked
(`codex exec fork`, `gpt-5.6-sol`, reasoning high, read-only sandbox, ~594k tokens). Sol was given
the repository and asked **what could reasonably change and what it would take to prove each change
worth it**. Advice only: no files were changed, nothing was built or run. The owner's stance: *"open
to big changes, but only if they prove themselves worthy."*

## Orchestrator verification (2026-09-15, against HEAD `cf9904e` and the UE 5.7 engine source)
Checked by reading, not by running anything.

| Sol's claim | Check |
|---|---|
| `FMath::Sin/Cos` reach the MSVC CRT on Win64 | **Verified.** `GenericPlatformMath.h:521/530` returns `sinf`/`cosf`; `FMicrosoftPlatformMathBase` (via `TUnrealPlatformMathSSE4Base`) and the SSE bases override neither. |
| Per-sample lookup machinery is ~5% exclusive, not the 27% inclusive figure | **Verified**, `PERF-SAMPLED.md` lines 11-13: `GetRange` 3.8%, `VF_ForEachSpatialCandidate` 1.2%. My round-one note "the largest gap is the loop direction" overstated it. |
| Parse-once switches are unsynchronised globals parsed on worker threads | **Verified**, `VoxelGenerator.cpp` (`VF_Parse*Switch` called in `GetDensityAt`). In practice every racer writes the same value; the real exposure is a live CVar change mid-generation. |
| `MemCrc32` over raw param structs; the comment says no false positive is possible | **Verified** (`VoxelGenerator.cpp` ~3165). A CRC32 collision is possible in principle (about 2^-32 per comparison); padding can only cause a spurious rebuild. Low probability. |
| Mesher dedups vertices by rounding position x100 | **Verified** (`VoxelMarchingCubesMesher.cpp` `GetOrCreateVertex`). Only amplifies an upstream numeric difference; not an independent source of divergence. |
| ARCHITECTURE says every landing gets a connector to (0,0); the code says a source-fit landing stays local | **Verified contradiction**: `ARCHITECTURE.md` ~518 vs `VoxelStrateManager.cpp` ~2108. One of them is wrong. |
| Lateral-region audit 4/16 | **Verified**, `ARCHITECTURE.md` ~122. That mechanism is explicitly "not ready to promote", so it is not live terrain. |
| Layout shuffle uses a stateful `FRandomStream` | **Verified** (`VoxelStrateManager.cpp` ~1155), after a deterministic sort. Deterministic; only order-coupled when content is added. |
| Coarse LOD derived from fine data would break the clipmap invariant | Agreed (`ARCHITECTURE.md` ~708). My round-one table wrongly listed it as a gap. |
| Noise lattice float-to-int, MXCSR rounding mode, multiplayer edit ordering | Not re-checked in detail; recorded as Sol's claims. Edit ordering matters once networking exists. |

---

## The prompt, as sent

# Phase two: now you get the real code

Thank you for the first answer. It was the most useful design input this project has had in a
while. The owner read it and his reaction was: **"what Sol proposed can be nice. I am open to big
changes, but only if they prove themselves worthy."** That is the question now: **what could reasonably be changed in this code, and what would it take to
prove each change worth it?**

**The earlier rule is lifted: you may now read and search files.** This remains a read-only
consultation:
- Reading commands only (`rg`, `Get-Content`, `git log/show/diff`). Do not write or edit any file.
  Do not build, compile, run tests, or launch Unreal. A single test run here takes up to an hour,
  and the machine is the owner's.
- Use absolute paths; your shell's working directory can drift.
- Your final message is the whole deliverable. Please list the files you actually read at the end.

## The code
Plugin root: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge` (git branch `experimental`, HEAD `cf9904e`).
Start with these, reading targeted sections rather than whole files; some are over 100 KB:
- `CLAUDE.md` (rules), then `CODEMAP.md` (§2 data flow, §3 symbol→file index, §6 gotchas).
- `ARCHITECTURE.md` §8, especially **§8.10, the performance invariants** that exist for measured
  reasons.
- `WORK-NEXT.md`, section "State after 2026-09-15" and "Open design questions for the owner".
- `PERF-SAMPLED.md`, section "Current-tip cost re-rank — 2026-09-15 (1d824aa)" (from line ~1669):
  the measured cost map. Two later commits cut a further ~12% (`a668833`, reach gating) and ~4%
  (`410788f`, per-chunk routing cache).
- `DESIGN-SOL-2026-09-15.md`: your first answer, plus **my** mapping of it onto this code (the
  "Orchestrator notes"). I wrote that mapping from memory of the code; check it and correct me.
- `AUDIT-SOL-2026-09-11.md` and `AUDIT-COLD-2026-09-14.md`: earlier read-only audits, so you don't
  repeat them.
- Hot code: `Source/VoxelForge/Private/VoxelGenerator.cpp` (`GetDensityAt`,
  `GetDensityWithParams`), `VoxelCaveMorphology.cpp` (tunnel SDF, world-space tunnel core, player-fit
  memo), `VoxelDensityOpStack.cpp` (operator stack, fused evaluator), `VoxelStrateManager.cpp`
  (strates, passages), the mesher / tile scheduling, and `VoxelForge.Build.cs` (`/fp:precise` is
  already set).

## What is measured (don't re-derive; do challenge it if the code says otherwise)
- In-game worker CPU for the standard static scenario is ~27.6 s, down from ~184 s at the start of
  this push, with the same world and the same capability. Measured on the headless game path with a
  stack sampler plus remove-one ablation.
- **Tried and rejected, please don't re-propose these without new evidence:**
  - Merging the warped tunnel SDF with the world-space tunnel core into one pass: the world core is
    load-bearing (its control points keep authored floor anchors before warp). Replacing it lost
    1,146 player-fit and 975 walk-reachable cells.
  - An always-on exact per-sample cull: +23% slower.
  - Cheaper value-noise worms: no gain. Worms off entirely saves only ~3%.
- The acceptance rules, which any proposal must live with:
  - **Determinism:** same build + same seed = same world on every machine. Bit-identity with
    *older* builds is NOT required, so a change that alters the field is allowed. Classifiers and
    skip proofs must never change the field.
  - **Capability:** the existing gate (player-fit 20,830 / walk-reachable 10,909 cells, connected,
    on the canonical scenario) must not regress.
  - **Performance:** judged on the headless GAME path, static and moving, comparing the final
    default build against the committed-tip DLL. A switch-off A/B inside the new build does not
    count.
  - The operator stack and the composer stay as the authoring language: worlds are composed, not
    hand-built.

## What we would like from you
1. **Correct my mapping.** For each row of the table in `DESIGN-SOL-2026-09-15.md`, say whether it
   is right, and give file:symbol evidence where it is wrong.
2. **Your ideas against this code.** For each idea from your first answer that survives contact with
   the code:
   - where it plugs in (files, symbols) and what it replaces or keeps;
   - its rough size;
   - its risks to capability, determinism, the composer and the §8.10 invariants;
   - its expected gain, grounded in the measured cost map. Say what share of the ~27.6 s it could
     touch, and mark each number measured or inferred.
3. **Rank them.** Then, for the top one or two big changes, design a **proof-of-worth ladder**:
   - Rung 1: the cheapest experiment that could kill the idea early. Ideally instrumentation only,
     e.g. how much time goes to per-sample candidate search, and how many candidates a sample sees.
   - Rung 2: a prototype scoped to one feature type, behind a console switch.
   - Rung 3: full adoption.
   Each rung needs an explicit, falsifiable kill criterion, and the numbers that would justify going
   on. The owner wants big changes to earn their place, so make the early rungs cheap and honest.
4. **Determinism audit of numerics on the world-deciding path.** We build with `/fp:precise`.
   Enumerate the remaining cross-machine hazards with file:line:
   - CRT transcendentals (`sin/cos/pow/exp/log/atan2/...`, including through `FMath`);
   - any runtime CPU-feature dispatch (SIMD paths chosen at runtime, ISPC);
   - float-to-int conversions at boundaries;
   - order-dependent accumulation across threads, uninitialised data, timing dependence.
   Give severity and the minimal fix for each. In particular: on UE 5.7 Win64, do `FMath::Sin/Cos`
   end up in the MSVC CRT (whose FMA3 variant is chosen at runtime), or in engine code? If engine
   source is not reachable from here, say so and say what you would check.
5. **What not to graft here**, and why.

This is advice only. Nothing will be changed as a result of this run until the owner has read your
answer and decided, so don't write code or patches. Describe what could reasonably change, and what
it would take.

Push back wherever the code shows my framing or my mapping is wrong. That is worth more than
agreement. Say plainly where you are unsure.


---

## Sol's answer, verbatim

# Executive conclusion

I would not replace VoxelForge’s representation, mesher, streaming model, or operator authoring language wholesale. The current design is already much closer to the “lazy compiled field” architecture from the first answer than the orchestrator notes imply, and the measured reduction from ~184 s to ~27.6 s shows that its major structural choices are viable.

Three changes remain worth serious consideration:

1. A tile-wide, feature-major lowering of the canonical tunnel evaluator. This is the only remaining architectural change with a plausible 8–18% total worker-CPU gain, but it has not yet earned implementation.
2. Gameplay reuse of exact density samples across adjacent tiles. The existing mesher already has most of the consumer-side plumbing, and the theoretical duplicate-sample ceiling is surprisingly large.
3. A canonical numeric ABI for world-deciding math. This is primarily correctness, not performance: `/fp:precise` does not currently close the determinism requirement.

I would also harden the traversal contract. The canonical capability gate passes, but the code and documentation do not support the stronger claim that every composed world has a structurally guaranteed entry-to-exit route.

All performance percentages below distinguish:

- **Measured:** directly present in the supplied profiles or commit evidence.
- **Inferred:** derived from those measurements; not yet benchmarked.

---

# 1. Correction of the orchestrator mapping

There is no table in `DESIGN-SOL-2026-09-15.md` at this HEAD. There are three mapping bullets at [DESIGN-SOL-2026-09-15.md:9](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/DESIGN-SOL-2026-09-15.md:9>). I have treated those as the requested rows.

| Orchestrator note | Verdict | Correction |
|---|---|---|
| Determinism risk from `FMath::Sin/Cos` and UCRT FMA3 dispatch | **Substantively correct; count is stale/ambiguous** | The risk is real and `/fp:precise` does not fix library implementations. The current source contains 42 direct `Sin/Cos` invocations in terrain generation if legacy and fused implementations are both counted, plus eight in deterministic content placement—not simply 34. |
| “The largest gap is the loop direction” | **Plausible, not proved** | Every tunnel sample does query a 2D spatial bucket and iterate candidates, but the exclusive lookup machinery was only about 5% in the newer profile. The old 27% figure was inclusive of the feature kernels. Feature-major batching may still win through SIMD, hoisting and locality, but candidate lookup alone cannot justify a rewrite. |
| Already shares built-first traversal, hash RNG, collision gate, partial narrow-band skipping | **Mixed** | Collision gating and narrow-band/reach proofs are real. Hash-keyed randomness is dominant but not universal. Traversal is structurally constructed for major archetypes, but the universal guarantee is not established, especially for fallback passage landings and lateral-region composition. |

## 1.1 Determinism note

The direct terrain-generation `Sin/Cos` sites are:

- [VoxelGenerator.cpp:1129](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelGenerator.cpp:1129>), `1178`, `1377`, `1456`, `1557`, `1612`, `4708`, `4738`, `5723`.
- [VoxelDensityOpStack.cpp:7507](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelDensityOpStack.cpp:7507>) and `7609`.
- [VoxelHeightOpStack.cpp:232](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelHeightOpStack.cpp:232>).
- [VoxelCaveMorphology.cpp:2931](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelCaveMorphology.cpp:2931>), `3266`, `6826`, `6843`, `7367`, `7487–7488`, `7520–7521`.
- [VoxelStrateManager.cpp:1830](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelStrateManager.cpp:1830>), `1993`, `2008`, `2021–2022`, `2032–2033`, `2045`, `2087`, `2142`.

The duplication matters: several calls belong to parallel legacy/fused implementations, so a raw count overstates unique runtime work while still accurately describing the audit surface.

The engine source resolves the uncertainty conclusively:

- `FMath` inherits `FPlatformMath` at [UnrealMathUtility.h:269](</E:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/Core/Public/Math/UnrealMathUtility.h:269>).
- Win64 aliases `FPlatformMath` to `FMicrosoftPlatformMathBase` at [WindowsPlatformMath.h:18](</E:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/Core/Public/Windows/WindowsPlatformMath.h:18>).
- The Microsoft class inherits the generic implementation and does not override `Sin` or `Cos` at [MicrosoftPlatformMath.h:12](</E:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/Core/Public/Microsoft/MicrosoftPlatformMath.h:12>).
- Generic `Sin` calls `sinf` and generic `Cos` calls `cosf` at [GenericPlatformMath.h:521](</E:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:521>) and [GenericPlatformMath.h:530](</E:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:530>).

So, on UE 5.7 Win64, `FMath::Sin/Cos` use the MSVC/UCRT functions. They do not use Unreal’s polynomial `FMath::SinCos(float)`, which is separate engine code at [UnrealMathUtility.h:752](</E:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/Core/Public/Math/UnrealMathUtility.h:752>).

Microsoft documents that the x64 CRT selects FMA3 implementations at startup based on CPU support and that the alternate algorithms can produce different results. [`_set_FMA3_enable` documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/get-fma3-enable-set-fma3-enable?view=msvc-170).

The existing cross-platform test already recognizes the libm hole at [VoxelForgeCrossPlatformTest.cpp:158](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/Tests/VoxelForgeCrossPlatformTest.cpp:158>), but it reports risk rather than enforcing an identical field digest. Therefore constraint 1 is not currently proved.

## 1.2 “Largest gap is loop direction”

What the note gets right:

- A cache builds 2D chunk buckets at [VoxelCaveMorphology.cpp:1325](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelCaveMorphology.cpp:1325>).
- Each sample calls `VF_ForEachSpatialCandidate`, including `GetRange`, at [VoxelCaveMorphology.cpp:1462](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelCaveMorphology.cpp:1462>).
- Tunnel SDF and world-core evaluation perform separate candidate walks at [VoxelCaveMorphology.cpp:7847](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelCaveMorphology.cpp:7847>) and [VoxelCaveMorphology.cpp:8042](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelCaveMorphology.cpp:8042>).
- The canonical fused path remains scalar per sample: `GetDensityAt` calls `GetDensityWithParams` at [VoxelGenerator.cpp:3386](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelGenerator.cpp:3386>).
- The mesher calls `GetDensityAt` once for every required grid sample at [VoxelMarchingCubesMesher.cpp:694](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelMarchingCubesMesher.cpp:694>).

What is overstated:

- The `1d824aa` sampler’s `VF_ForEachSpatialCandidate` share was inclusive of work executed inside the visitor.
- The later `410788f` exclusive attribution puts `GetRange` around 3.8% and `VF_ForEachSpatialCandidate` itself around 1.2%.
- Thus the directly removable search overhead is approximately 5% of the old profile, not 27%.
- The expensive swept-tunnel work remains inside those callbacks. A loop inversion must make that work cheaper—not merely remove the lookup—to justify itself.

“Coarse LOD from fine data is absent” is factually true but not evidence of a gap. Direct evaluation at every LOD is a deliberate clipmap invariant: it maintains roughly constant tile complexity and does not force far terrain to materialize at LOD0. See [ARCHITECTURE.md:708](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/ARCHITECTURE.md:708>).

“Shared halo samples are absent” needs qualification:

- Every tile already has a single local density grid plus a one-sample margin; cell corners and normals are not resampled. See [VoxelMarchingCubesMesher.cpp:656](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelMarchingCubesMesher.cpp:656>) and [ARCHITECTURE.md:652](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/ARCHITECTURE.md:652>).
- `FVoxelSharedDensityGrid` already exists and the mesher can consume it at [VoxelMarchingCubesMesher.cpp:660](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelMarchingCubesMesher.cpp:660>).
- It is populated only by the editor exploration commandlet, culminating in `SetSharedDensityGrid` at [VoxelForgeExploreCommandlet.cpp:1905](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForgeEditor/Private/VoxelForgeExploreCommandlet.cpp:1905>).
- Gameplay does not share density samples across neighboring tiles.

So the missing opportunity is specifically **cross-tile gameplay reuse**, not basic grid or halo caching.

## 1.3 “Already shared”

### Traversal: partial, not universal

The current system has real construction-first features: walk-critical tunnel forests, D-floors, option-B winding and structural posts. That part of the mapping is fair.

But two facts prevent me from calling the hard guarantee closed:

1. `ARCHITECTURE.md` says every landing receives a deterministic connector to the origin landing at [ARCHITECTURE.md:518](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/ARCHITECTURE.md:518>). Current code explicitly says the opposite: a source-fit landing remains local and no radial connector is synthesized at [VoxelStrateManager.cpp:2108](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelStrateManager.cpp:2108>). The document is stale or the implementation regressed from its stated contract.

2. The lateral-region audit remains at 4/16 connected cases after landing, including region-zero controls, at [ARCHITECTURE.md:122](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/ARCHITECTURE.md:122>).

`VF_SuggestLandingPoint` can make a local landing safe when it succeeds, but the documentation says unsupported/no-footing cases fall back to historical random reach at [ARCHITECTURE.md:512](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/ARCHITECTURE.md:512>). That is not a construction proof.

The canonical 20,830 / 10,909 gate is valuable regression protection. It is not a proof across seeds, composer rolls and archetype mixtures.

### Randomness: mostly hash-keyed

Most feature decisions are keyed hashes. The layout shuffle still uses one stateful `FRandomStream` after sorting the asset pool at [VoxelStrateManager.cpp:1145](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelStrateManager.cpp:1145>) and [VoxelStrateManager.cpp:1157](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelStrateManager.cpp:1157>).

That is deterministic for the same build and pool. It is nevertheless order-coupled: adding one pool choice can shift every later choice. It therefore falls short of the first answer’s “stable ID plus counter-based randomness” ideal, though it does not presently violate the stated cross-machine rule.

### Collision gate: correct

The implementation now treats actual collision-cook completion as admission authority. This is stronger than speculative prefetch. It satisfies the hard safety part even under overload by stopping motion.

What remains from the first answer is UX improvement: predictive fall/shaft prefetch could make the gate engage less often. It is not required for correctness.

### Narrow bands: correct and substantial

The sealed-solid proof, tunnel-core reach, passage-carving reach, worm block proof, cached topology and lateral-region preparation all exist. They are not “partial” in the sense of being superficial; §8.10 records a considerable exact-skip system.

---

# 2. Ideas that survive contact with the code

## Ranked recommendations

| Rank | Change | Share of 27.6 s it could touch | Expected net gain | Rough size |
|---|---|---:|---:|---:|
| 1 | Tile-wide, feature-major lowering for tunnels | **35–50% inferred**: 9.7–13.8 s | **8–18% inferred**: 2.2–5.0 s | 0.8–1.5 KLOC prototype; 3–6 KLOC production |
| 2 | Cross-tile exact density-grid reuse | Up to **22.9% theoretical/inferred**: 6.3 s; actual unknown | **5–12% inferred** if reuse is high | 0.6–1.2 KLOC prototype; 1.5–3 KLOC production |
| 3 | Canonical numeric ABI | Probably under 5% runtime touched; correctness is the value | Approximately neutral, perhaps −1% to +1% inferred | 0.3–0.8 KLOC plus tests |
| 4 | Traversal-plan contract hardening | 0% of static worker time | Correctness, not speed | 1–3 KLOC plus seed-matrix tests |
| 5 | Exact base-density reuse and canonical edit compaction | **0% measured** in the static scenario | Potentially large only after repeated edits; unmeasured | 1.5–3 KLOC |
| 6 | Deadline/fall-aware prefetch | Moving-streaming work only; unknown | Gate-engagement and latency improvement, not established CPU savings | 1–2 KLOC |
| 7 | Topology-aware underground visibility | Far streaming only; unknown | Speculative | Large |

The `1d824aa` percentages are historical measured shares from a 29.30 s build, not additive and not exact for the current 27.6 s build. Scaling them onto 27.6 s is explicitly inferred.

## 2.1 Tile-wide, feature-major lowering

### Where it plugs in

Add a tile/grid evaluation entry point beside:

- `UVoxelGenerator::GetDensityAt` at [VoxelGenerator.cpp:2799](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelGenerator.cpp:2799>).
- `UVoxelGenerator::GetDensityWithParams` at [VoxelGenerator.cpp:3921](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelGenerator.cpp:3921>).
- The mesher grid-fill loop at [VoxelMarchingCubesMesher.cpp:694](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelMarchingCubesMesher.cpp:694>).

Conceptually:

```text
composer/operator stack
        ↓ lowering
canonical tile program
        ↓
tile-local SoA buffers
        ↓
existing density grid
        ↓
existing two-pass marching cubes
```

It should replace only the repeated scalar execution of the canonical tunnel-network graph. It should keep:

- `FVoxelOpStack` as the authoring language.
- `BuildTunnelNetworkStack` and the fused-evaluator designation.
- `FChunkSDFCache` and its search-box validity.
- The warped tunnel SDF and world-space core as separate semantic stages.
- The common structural tail, boundary seal and edit layer.
- Scalar fallback for custom recipes, fractional point queries and unsupported stacks.
- The existing marching-cubes grid, margin and two-pass cell loop.

The production algorithm I would try is:

1. Prepare the same chunk/tile context once.
2. Allocate thread-local SoA arrays for density, warped coordinates, nearest-room ownership and world-core handoff flags.
3. Compute sample-invariant and column-invariant data in batches.
4. Iterate rooms/tunnels in existing stable feature-index order.
5. Clip each feature’s conservative 3D influence bounds to the tile lattice.
6. Traverse X-contiguous spans and evaluate four or eight positions together where the formula permits.
7. Apply smooth unions in the same per-sample feature order.
8. Run later operators and the structural tail in their canonical order.
9. Write the existing `DensityGrid`.

The index builder inserts item indices in ascending feature order at [VoxelCaveMorphology.cpp:1445](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelCaveMorphology.cpp:1445>). Maintaining that order matters because smooth-min and other floating operations are order-sensitive.

### Measured basis

Historical remove-one shares:

- Tunnel core: **20.84% measured**, corresponding to 5.75 s when scaled to 27.6 s.
- Tunnel SDF: **12.64% measured**, corresponding to 3.49 s.
- Room SDF: **4.24% measured**, corresponding to 1.17 s.
- Cave warp: **6.68% measured**, corresponding to 1.84 s.
- Passage carving/structure can be incorporated later.

The tunnel core plus tunnel SDF therefore account for **33.48% measured historically**, or 9.24 s scaled to the current total. A batch evaluator cannot eliminate that geometry; it can only reduce its loop, load, branch and arithmetic cost.

The newer exclusive profile says lookup machinery itself was approximately **5% measured historically**, about 1.38 s scaled. This is why “invert the loops” alone is not enough. The hoped-for gain must come from:

- Hoisting tunnel metadata.
- Avoiding repeated lambdas/cache lookups and repeated world-coordinate construction.
- SIMD across adjacent X samples.
- Reusing centerline segment data across samples.
- Better sequential reads and writes.
- Clipping feature loops in 3D rather than retrieving a 2D bucket and rejecting on Z for every sample.

### Risks

- **Capability:** high. The core/SDF handoff and room-floor ownership are subtle. The rejected merge shows this is load-bearing.
- **Determinism:** medium-high. SIMD must use the same fixed instruction path on every x64 machine. Any conservative span skip must be reference-checked and field-identical.
- **Composer:** low if this remains a lowering backend. High if it becomes a separate hand-authored terrain definition.
- **§8.10:** high implementation risk. It must preserve search-box validity, lateral per-chunk preparation, reach masks, the margin grid, cancellation points, TLS reuse and direct evaluation at each LOD.
- **Maintenance:** high if the scalar and batched semantics fork. Shared primitive functions and generated/lowered stage descriptions are preferable to copying thousands of lines.

I would not generalize it to every archetype first. TunnelNetwork/Underwater already have a canonical fused designation and dominate the measured cost.

## 2.2 Exact density sharing across gameplay tiles

This is a more concrete surviving piece of the “shared bricks/halos” proposal than a full sparse-brick rewrite.

### Why it may be larger than it looks

A normal full-resolution tile has 32 cells but samples a 35³ grid because of the cell boundary and one-point normal margin.

For an ideal dense array of adjacent same-LOD tiles, the asymptotic duplicate fraction is:

```text
1 − 32³ / 35³ = 23.6%
```

This is an inferred theoretical ceiling. Actual reuse will be lower because of clipmap shell boundaries, Z bands, cancellation and tile completion order.

The old sampled profile attributed **96.9% measured, inclusive** to `GetDensityAt`. If duplicated coordinates had average cost, the absolute theoretical CPU ceiling would be approximately:

```text
23.6% × 96.9% = 22.9% of total
```

That is 6.3 s of the 27.6 s total. It is not an expected gain; it is the reason this deserves instrumentation.

### Where it plugs in

- Existing consumer support: `FVoxelSharedDensityGrid` in [VoxelMarchingCubesMesher.h:25](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Public/VoxelMarchingCubesMesher.h:25>).
- Existing copy path: [VoxelMarchingCubesMesher.cpp:660](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelMarchingCubesMesher.cpp:660>).
- Scheduler and tile lifetime: `AVoxelWorld::LoadTile`, task completion and cancellation paths.
- Edit revision: `UVoxelDiffLayer::ModsVersion` and the snapshot mechanism.

I would prototype an **opportunistic completed-neighbor slab cache**, not a global per-sample hash table:

- Store exact float boundary slabs from recently completed tiles.
- Key by generator owner, layout/generation epoch, final-field edit revision, sample step and integer world-lattice bounds.
- Before sampling a tile, copy all available overlap slabs with simple block loops.
- Evaluate only missing rectangular regions.
- Never wait for a neighboring tile.
- Evict outside the desired/in-flight neighborhood.

The current `SetSharedDensityGrid` member pointer is suitable for the single-threaded editor commandlet, not for concurrent gameplay tasks. Production should pass immutable donor views with each mesh request, not mutate shared mesher state.

Do not reuse the quantized density-volume capture for meshing. Its bytes are sufficient for lighting volume capture, not exact marching-cubes interpolation.

### Risks

- **Streaming:** a support tile must never wait for a batch or neighbor.
- **Revision correctness:** an edit or layout rebuild must prevent reuse of stale final densities.
- **Memory:** retaining complete 35³ float grids is ~171 KB per tile. Store bounded slabs or short-lived grids, not every loaded tile indefinitely.
- **Concurrency:** avoid global locks in the sample loop.
- **Determinism:** exact copied floats are safe if the key is complete. A stale cache hit is a geometry error.
- **§8.10:** preserve task cancellation and the thread-local destination grid; copying merely replaces a subset of its fill operations.

Expected net gain is **5–12% inferred**, conditional on actual ready-before-use reuse being high. It may also fail completely if most neighboring tasks execute concurrently and finish too late to donate samples.

## 2.3 Canonical numeric ABI

This is the surviving portion of the first answer’s WorldPlan recommendation with the highest correctness value.

The code already has a substantial plan/layout layer, cached topology, per-strate parameters and operator recipes. I would not introduce a second “WorldPlan” representation. I would add:

- A generation format/version ID.
- Stable feature IDs for topology objects and composer choices.
- Canonical deterministic `SinCos`, normalization and checked coordinate conversions.
- Golden plan, density and mesh digests.
- An explicit floating-environment contract for worker tasks.

This touches very little of the 27.6 s. The per-sample `Sin` calls may be measurable, but the topology and passage calls are cold. Expect approximately neutral performance.

## 2.4 Traversal contract hardening

The least invasive structural fix is to make passage creation return one of two outcomes:

- A landing with a certificate identifying the existing structural feature/component to which it belongs.
- A newly constructed explicit connector included in the mandatory route graph.

A failed `VF_SuggestLandingPoint` should cause a deterministic retry/recomposition or explicit connector. It should not silently fall back to an unproved random reach.

For lateral compositions, generate and reserve the mandatory entry-to-exit spine at the parent level before regional creative stacks. Regional terrain may join or decorate it but not own its existence.

This preserves the composer and adds no material density-loop cost. The proof is a seed/archetype/composition matrix, not merely the canonical scenario.

## 2.5 Edit compaction and base-grid reuse

The current edit layer duplicates an edit into each affected chunk and evaluates the list serially at [VoxelDiffLayer.cpp:361](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelDiffLayer.cpp:361>). There is no compaction into canonical per-brick deltas.

This does not touch the standard static 27.6 s scenario: **0% measured**.

For mining-heavy sessions, I would eventually keep:

- Immutable generated base samples for a recently remeshed edited tile.
- A server-sequenced edit stream.
- Canonical per-tile delta compaction after a threshold.
- Separate base-generation, edit, mesh and collision revision numbers.

Do this only after creating an edit-heavy benchmark. Depending on edit-list length, it might remove 30–80% of edit-layer work or much of repeated base evaluation, but that is wholly inferred and should not be sold using the static benchmark.

---

# 3. Proof-of-worth ladders

## Change A: tile-wide feature-major tunnel evaluation

### Rung 1 — Instrument the current evaluator

Use the existing counters:

- `CaveRoomCandidates/Evaluated`
- `CaveTunnelCandidates/Evaluated`
- `TunnelCoreCandidates/Evaluated`

They are recorded in [VoxelCaveMorphology.cpp:7765](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelCaveMorphology.cpp:7765>), [VoxelCaveMorphology.cpp:7844](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelCaveMorphology.cpp:7844>) and [VoxelCaveMorphology.cpp:8042](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelCaveMorphology.cpp:8042>), then logged per tile at [VoxelWorld.cpp:4961](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelWorld.cpp:4961>).

Add profile-only counters for:

- `GetRange` calls/hits/fallbacks and cycles.
- Candidate visits rejected by 3D AABB.
- Candidate visits rejected by bounding sphere.
- Actual swept-tunnel evaluations.
- Centerline segment iterations.
- For each tile, the predicted feature-major lattice visits obtained by clipping every feature’s conservative 3D bounds to that tile.
- Separate static/moving and LOD0/all-LOD totals.

Do not time every sample with a heavyweight timer; aggregate cycles around whole loops or rely on sampling.

**Kill immediately if:**

- Exclusive candidate/rejection machinery remains below 6–7% of worker time, and
- Predicted feature-major visits are at least 80% of current expensive evaluations, and
- There is no clear X-lane SIMD opportunity in the swept kernel.

That combination caps the likely gain below what a large rewrite deserves.

**Proceed if either:**

- Conservative feature spans reduce candidate/kernel visits by at least 2×, or
- The combined measured loop/branch/kernel share with realistic SIMD opportunity is at least 12% of total worker time.

Also require projected scratch below roughly 2 MB per worker for a single-tile prototype.

### Rung 2 — Tunnel SDF only

Behind a diagnostic console switch:

- Batch the warped tunnel SDF.
- Leave `EvaluateTunnelCoreWorld` entirely unchanged.
- Preserve room-mouth ownership and feature order.
- Keep scalar evaluation as the reference path.
- Add a debug mode comparing every produced sample with the reference field.

The tunnel SDF had a **12.64% measured historical share**, so the prototype has enough budget to show a useful result without touching the load-bearing core.

**Kill immediately on:**

- Player-fit below 20,830.
- Walk-reachable below 10,909.
- Loss of connected status.
- Any nondeterministic digest across repeated runs or machines.
- A span/proof skip that changes a reference density bit.
- A violation of the two-region cache window, margin grid or cancellation contracts.

**Performance kill criterion:**

- Less than 3% total worker-CPU improvement in either static or moving, or
- More than 3% regression in generation/collision-ready p95.

**Justification to continue:**

- At least 5% total worker-CPU improvement in both static and moving, with no material p95 or memory regression.

The switch-on/off result is only an internal decision aid. It does not satisfy final acceptance.

### Rung 3 — Separate batch stages for world core and passage work

Add the world-space core as a second, semantically separate batch stage. Do not merge it with the warped SDF. Then consider passage carving/structure if the same infrastructure applies.

Full adoption requires:

- Default-on build compared against the untouched `cf9904e` DLL.
- At least 10% worker-CPU improvement in both static and moving, or at least 8% plus a material collision-ready/p95 improvement.
- Canonical capability counts and connection.
- Skip/reference tests clean.
- Memory and task-cancellation behavior within existing bounds.
- No custom-recipe or lateral-composition semantic fork.

## Change B: cross-tile exact density reuse

### Rung 1 — Count exact duplicate opportunities

Record, for every generated tile:

- Origin, step, sampled Z band and generation/edit revision.
- Task start and completion order.
- Count of requested lattice coordinates.
- Count of distinct lattice coordinates across the scenario.
- Count whose donor tile had already completed before the consumer started.
- Reuse grouped by LOD, same/cross-LOD, static/moving and density-cost bucket.

This can be computed offline from a compact tile trace; there is no need for a runtime hash set of every sample in the production path.

**Kill if:**

- Exact duplicate coordinates are below 12% of all density calls, or
- Fewer than half of duplicates have a completed donor in time, or
- Time-weighted eligible duplicates represent less than 5% of worker CPU.

**Proceed if:**

- At least 15% of calls are exact duplicates and at least 60% are available opportunistically, or
- The conservative time-weighted saving ceiling exceeds 10% of total worker CPU.

### Rung 2 — LOD0 completed-neighbor slabs

Prototype only LOD0:

- Exact float slabs.
- Immutable per-request donor views.
- No waiting.
- No cross-LOD reuse.
- Strict owner/layout/edit revision key.
- Bounded memory, preferably 32–64 MB.
- Missing samples follow the current path.

**Kill immediately on any field/mesh digest difference.** This should be a pure reuse, not a field-changing implementation.

**Performance kill criterion:**

- Less than 4% worker-CPU improvement in either static or moving.
- More than 3% collision-ready or generation p95 regression.
- Memory above the agreed cap.
- Significant loss of task concurrency.

**Proceed if:**

- At least 6% worker-CPU improvement in both scenarios.
- No readiness regression.
- No stale-hit failure under edit/rebuild/cancellation stress.

### Rung 3 — Full same-step gameplay reuse

Extend to same-step tiles at all useful clip levels, with revisioned invalidation and bounded eviction. Cross-LOD reuse is optional and should be separately justified.

Adopt only if the final default build beats `cf9904e` by at least 7–8% overall or produces a slightly smaller CPU gain plus a clear collision-readiness improvement. Remove the diagnostic switch only after the committed-DLL comparison.

---

# 4. Numeric determinism audit

## 4.1 Critical: CRT `sin/cos`

**Severity: critical.**

These functions affect:

- Passage control points and landing directions.
- Tunnel winding and pinch orientation.
- Room/feature placement.
- Per-sample line/rib/surface detail.
- Walkability thresholds.

The UCRT can select different implementations in the same Win64 binary based on CPU features. Windows and Linux libm implementations may also differ.

Minimal fix:

1. Introduce one canonical float `SinCos` implementation with explicitly bounded input, fixed range reduction and fixed operation grouping.
2. The UE float `FMath::SinCos` polynomial is a reasonable prototype because it is engine arithmetic rather than CRT dispatch. I would wrap or vendor it so the numeric contract is explicit.
3. Replace paired `Sin`/`Cos` calls together.
4. Use the same canonical sine output for individual sine-only sites.
5. Pin golden input/output bit patterns and world-plan/field digests.
6. Retune if necessary; a new build may change the field.

Calling `_set_FMA3_enable(0)` is not a complete fix. It is process-global, Windows-specific and does not align UCRT with glibc.

## 4.2 Medium: `sqrt` and implicit vector normalization

`FMath::Sqrt` also reaches `sqrtf` at [GenericPlatformMath.h:552](</E:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:552>). On x64 this will commonly become a hardware square-root instruction, which is much less alarming than `sin/cos`, but the source-level contract still delegates to the platform library/compiler.

World-deciding examples include:

- Morphology topology/walkability: [VoxelCaveMorphology.cpp:639](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelCaveMorphology.cpp:639>), `785`, `2171`, `2188`, `3019`, `3027`, `4919`, `5101`, `5355`.
- Tunnel/room field evaluation: `7704`, `8356`, `8550`.
- Passage/layout geometry: [VoxelStrateManager.cpp:666](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelStrateManager.cpp:666>), `2393`, `2406`, `3701`, `3804`.
- Composer decisions: [VoxelStrateComposer.cpp:1572](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelStrateComposer.cpp:1572>), `1606–1608`, `4141`, `4172`, `4662–4665`, `4849`, `5016–5018`.
- Numerous SDF sites in `VoxelGenerator.cpp` and `VoxelDensityOpStack.cpp`.
- `FVector::Size`, `Dist` and `Normalize`, for example passage direction normalization at [VoxelStrateManager.cpp:2085](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelStrateManager.cpp:2085>).

Minimal fix:

- Replace square-root comparisons with squared comparisons where mathematically equivalent.
- Canonicalize only the remaining square roots that affect topology, integer conversion or threshold decisions.
- Verify emitted Win64 instructions and run golden vectors before attempting to replace every SDF square root. I am not claiming a demonstrated cross-CPU `sqrt` divergence here.

No direct runtime uses of `FMath::Pow`, `Exp`, `Log`, `Atan`, `Atan2` or `Tan` were found in the plugin generation sources. That does not rule out transitive engine/library use, but there are no direct call sites to fix.

## 4.3 High: runtime CPU state and dispatch

### Confirmed dispatch

- UCRT FMA3 `sin/cos`: critical, as above.

### Plugin SIMD

`VoxelNoise` selects scalar versus SSE at compile time through `VF_NOISE_USE_SIMD` at [VoxelNoise.h:35](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Public/VoxelNoise.h:35>). The hot implementation uses explicit SSE4.1 at [VoxelNoise.h:248](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Public/VoxelNoise.h:248>).

I found no plugin runtime CPUID dispatch and no ISPC path. The same Win64 build therefore takes the same plugin noise implementation on all supported x64 CPUs.

The “scalar and SIMD are bit-identical” claim at [VoxelNoise.h:20](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Public/VoxelNoise.h:20>) should still have a pinned exhaustive/random corpus. It is a claim worth continuously testing, not merely documenting.

### MXCSR rounding mode and FTZ/DAZ

UE’s integer conversions inherit SSE implementations such as `FloorToInt32` and `RoundToInt32` at [UnrealPlatformMathSSE.h:86](</E:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/Core/Public/Math/UnrealPlatformMathSSE.h:86>). Several use rounding-mode-sensitive conversion instructions and assume the normal environment.

Another library changing a worker thread’s MXCSR rounding mode could change world-to-cell decisions. FTZ/DAZ can likewise alter very small intermediates.

Minimal fix:

- At generation-task entry, assert or install a canonical MXCSR configuration and restore the previous state at exit.
- Prefer explicit rounding instructions or checked conversion helpers for canonical boundaries.
- Test all supported task threads, not only the game thread.

## 4.4 Critical/high: float-to-integer boundaries

### Noise lattice conversion

[VoxelNoise.h:96](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Public/VoxelNoise.h:96>) floors a float and casts it directly to `int32`. The SIMD version uses `_mm_cvttps_epi32` at [VoxelNoise.h:255](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Public/VoxelNoise.h:255>).

For NaN or out-of-range values, scalar C++ conversion and SSE conversion do not provide a suitable canonical world rule.

**Severity: high**, especially with `WorldRadiusVoxels == 0` permitting an unbounded world.

Fix:

- Define a supported integer-coordinate envelope.
- Reject or clamp malformed/out-of-range queries before noise evaluation.
- Use one checked floor/wrap policy shared by scalar and SIMD paths.
- Test the boundary values and their immediate float neighbors.

### Chunk routing

`GetDensityAt` converts public float coordinates to chunk coordinates without a finite/range guard at [VoxelGenerator.cpp:2836](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelGenerator.cpp:2836>).

Internal mesher calls are bounded integer coordinates, but other callers need not be.

Fix:

- Give the mesher an integer-lattice entry point.
- Keep the public float query checked.
- Decide explicitly what happens outside representable chunk space.

The edit layer already demonstrates the correct pattern at [VoxelDiffLayer.cpp:220](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelDiffLayer.cpp:220>).

### Procedural cell selection

`VoxelCaveMorphology.cpp` contains many `FloorToInt` cell selections, including the placement/cache families around `3902–3903`, `3954–3956`, `4038–4039`, `4180–4181`, `4554–4555`, `4683–4684`, `4881–4882`, `5085–5086`, `5195–5196` and `6055–6058`.

These are safe only while inputs remain finite and representable. They should consume the same declared coordinate envelope.

### Conservative skip boundaries

Passage and box proofs use float-to-int lattice conversions, for example around `VoxelStrateManager.cpp:3333–3334`. These are lower severity if they are genuinely one-sided: disagreement must cause a missed skip, never omitted geometry.

The better formulation is integer/rational lattice `ceil`/`floor` from validated bounds, with explicit outward rounding.

### Mesh vertex identity

The mesher deduplicates a vertex by rounding its interpolated world position multiplied by 100 at [VoxelMarchingCubesMesher.cpp:154](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelMarchingCubesMesher.cpp:154>).

A small numeric difference crossing a half-centimetre rounding boundary can change vertex sharing and triangle/index output even if the surface samples remain on the same side.

**Severity: medium-high.**

Minimal fix: identify marching-cubes vertices canonically by integer cell coordinate plus edge axis, not rounded interpolated position. That is both cheaper and independent of interpolation rounding.

## 4.5 High: worker-side global races

The following “parse once” flags and their backing values are ordinary globals accessed from worker generation:

- Generator flags at [VoxelGenerator.cpp:47](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelGenerator.cpp:47>), `73`, `98`, `124`, `143`, `202`.
- The parsers are invoked inside every `GetDensityAt` at [VoxelGenerator.cpp:2820](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelGenerator.cpp:2820>).
- Mesher flags at [VoxelMarchingCubesMesher.cpp:20](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelMarchingCubesMesher.cpp:20>) and `42`, invoked at `97–98`.

Multiple first-time worker calls can read/write the booleans concurrently. `FAutoConsoleVariableRef` can also mutate backing integers while workers read them. That is C++ data-race undefined behavior.

Minimal fix:

- Parse command-line state once on the game/module initialization thread before generation begins.
- Construct an immutable generation-policy snapshot.
- Copy that snapshot into each task or generation epoch.
- A live CVar change that affects the field must create a new epoch and rebuild; it must not alter active workers in place.
- Diagnostics that are intentionally live should use atomics and must not influence the canonical field.

## 4.6 Critical for multiplayer edits: order-dependent accumulation

Edits are appended to per-chunk arrays at [VoxelDiffLayer.cpp:253](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelDiffLayer.cpp:253>) and accumulated serially in insertion order at [VoxelDiffLayer.cpp:374](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelDiffLayer.cpp:374>).

Floating addition is order-dependent. If clients apply replicated edits in packet-arrival order, their density fields can differ.

Minimal fix:

- Give every accepted edit a monotonic authoritative server sequence.
- Buffer out-of-order edits and evaluate them in that sequence.
- Include sequence ranges in edit/density cache revisions.
- During compaction, use a canonical order or quantized/fixed-point delta representation.
- Never parallel-reduce floating edit contributions without a deterministic reduction tree.

Current local serial evaluation is stable if every machine receives the exact same order. The network contract must guarantee that.

## 4.7 Medium: raw-memory fingerprints and padding

The code hashes raw parameter structs using `FCrc::MemCrc32` at:

- [VoxelGenerator.cpp:3170](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelGenerator.cpp:3170>) and `3842`.
- [VoxelDensityOpStack.cpp:1116](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelDensityOpStack.cpp:1116>), `1119` and `3693`.

The accompanying comments say a raw-memory CRC cannot produce a false positive. That is incorrect:

- Padding may be indeterminate, yielding nondeterministic false misses.
- CRC32 can collide, yielding a false hit.
- If the fingerprint is the only cache identity, a false hit can reuse terrain computed from different parameters.

Minimal fix:

- Hash fields individually in declared canonical order.
- Normalize bools, floats and zero.
- Prefer 64- or 128-bit hashes.
- On a hash match, compare the complete canonical key before reusing semantic data.

This is probably a low-frequency failure, but an absolute determinism/correctness contract should not use “CRC collisions cannot happen” as an invariant.

## 4.8 Stateful randomness and ordering

`FRandomStream` layout shuffle is serial and deterministic for the current sorted pool, so it is not presently a cross-thread race.

Its weakness is evolutionary stability: inserting a pool member changes subsequent random consumption. Since older-build identity is not required, this is not urgent. Stable per-slot keyed selection becomes useful if saved plans, live content packs or replicated compact layouts must survive content additions predictably.

Current spatial candidate iteration is serial and ordered. I found no current parallel reduction of SDFs or edit values. A feature-major rewrite must preserve per-sample feature order or deliberately become a field-changing new canonical evaluator.

## 4.9 Uninitialized data and timing

I inspected the important `SetNumUninitialized` sites and found no confirmed uninitialized read:

- Spatial-index bound arrays are filled for every valid item before use at [VoxelCaveMorphology.cpp:1343](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelCaveMorphology.cpp:1343>).
- Index storage is completely filled from prefix offsets at `1443–1455`.
- The mesher samples only the Z band subsequently read; this is documented at [VoxelMarchingCubesMesher.cpp:680](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/Source/VoxelForge/Private/VoxelMarchingCubesMesher.cpp:680>).

The raw struct padding used by `MemCrc32` is the confirmed uninitialized-byte concern.

I found no density value chosen by wall-clock timing. Task timing controls which tiles become ready first, but epoch, cancellation and collision gates prevent obsolete work from becoming canonical terrain. Cache identity and edit sequencing are the places where timing could become semantic if keys/order are incomplete.

---

# 5. What I would not graft onto this code

- **No wholesale persistent sparse base-density brick store.** It would duplicate a pure procedural evaluator, add large revision/save/memory machinery and invalidate the current direct-LOD clipmap advantage. Transient tile buffers and edited-brick deltas capture most of the value.

- **No coarse LOD derived from mandatory fine voxels.** It would make far terrain require fine generation and undermine the constant-cell-count clipmap at [ARCHITECTURE.md:708](</E:/Projet Unreal/VoxelM/Plugins/VoxelForge/ARCHITECTURE.md:708>).

- **No min/max octree classifier as another always-on front end.** The outer classifier and exact per-sample cull already demonstrated that extra proof work can lose. Bounds should cheaply clip batch spans, not become a parallel evaluator.

- **No warped-SDF/world-core merge.** The rejected capability loss demonstrates that they express different geometry. Batch them separately.

- **No value-noise worm substitution or worm-focused rewrite.** Worm removal is only about 3%; that budget cannot support architectural work.

- **No general-purpose interpreted CSG evaluator.** The project has already paid to lower the canonical stack. General composition belongs in the authoring IR, not the sample loop.

- **No shell/B-rep base terrain or local shell-to-voxel collars.** Arbitrary noise, junctions, mixed archetypes and mining make the watertight seam problem much larger than the likely benefit.

- **No dual-contouring/transvoxel or mesh-extractor rewrite without an extractor-specific profile.** Density evaluation is the measured bottleneck. Changing topology would risk capability and seams while attacking an unmeasured target.

- **No GPU-authoritative field or meshing.** Cross-vendor bit identity remains unproved. GPU rendering/culling can be non-authoritative later.

- **No global concurrent hash map for individual density samples.** Per-sample lookup/locking could repeat the always-on-cull failure. Reuse should operate on rectangular slabs or immutable tile buffers.

- **No actor/UObject per brick.** Current plain worker data and game-thread publication are the right split.

- **No second WorldPlan/composer hierarchy.** `FStrateLayout`, recipes, cached topology and the operator stack already constitute one. Add stable IDs and contracts to those structures.

- **No conservative collision proxy unless gate stalls remain a real UX problem.** The current cook-complete admission gate already supplies correctness. A second proxy system needs a user-visible problem and measurements.

- **No claim that generated traversal remains guaranteed after arbitrary edits.** If that becomes a requirement, it needs an explicit server product rule: protect, reject or repair the final route.

---

# Bottom line

The owner’s “prove it worthy” stance is appropriate. The code has reached the point where another clever per-sample condition is more likely to lose than win.

I would authorize three preliminary efforts:

1. Instrument feature-major work potential using the existing candidate/evaluated counters.
2. Trace exact duplicate density coordinates and ready-before-use donors across gameplay tiles.
3. Close the numeric ABI hole, beginning with a FMA3-on/off and cross-platform golden corpus.

Only the first two are performance bets. Neither should become a production rewrite unless it clears its early kill criteria. The numeric work is different: the code currently documents, rather than satisfies, the hard field-determinism requirement.

# Files read

No files were changed. I did not build, run tests or launch Unreal.

Targeted sections read:

- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\CLAUDE.md`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\CODEMAP.md`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\ARCHITECTURE.md`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\WORK-NEXT.md`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\PERF-SAMPLED.md`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\DESIGN-SOL-2026-09-15.md`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\AUDIT-SOL-2026-09-11.md`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\AUDIT-COLD-2026-09-14.md`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\fable-idea.md`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\VoxelForge.Build.cs`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelCaveMorphology.cpp`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelDensityOpStack.cpp`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelStrateManager.cpp`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelMarchingCubesMesher.cpp`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelDiffLayer.cpp`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Public\VoxelNoise.h`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Public\VoxelDensityProfile.h`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Public\VoxelMarchingCubesMesher.h`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\Tests\VoxelForgeCrossPlatformTest.cpp`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForgeEditor\Private\VoxelForgeExploreCommandlet.cpp`

Files inspected through targeted symbol searches:

- `VoxelContentManager.cpp`
- `VoxelHeightOpStack.cpp`
- `VoxelStrateComposer.cpp`
- `VoxelStrateMeasure.cpp`
- `VoxelCaveMorphology.h`
- `VoxelDensityPrimitives.h`
- `VoxelHeightOp.h`
- `VoxelPassageGeometry.h`
- `VoxelStrateTypes.h`
- `VoxelForgeComposerStructureRollTest.cpp`
- `VoxelForgePassageOpenSpaceTest.cpp`
- `VoxelForgeScaleDiagnosisTest.cpp`
- `VoxelForgeStrateConnectivityTest.cpp`
- `VoxelForgeWormBlockSkipTest.cpp`

UE 5.7 engine source read:

- `GenericPlatformMath.h`
- `UnrealMathUtility.h`
- `MicrosoftPlatformMath.h`
- `WindowsPlatformMath.h`
- `UnrealPlatformMathSSE.h`
- `UnrealPlatformMathSSE4.h`

I also inspected git metadata/diffs for `1d824aa`, `a668833` and `410788f`.