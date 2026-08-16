> ## ⚠️ REVIEWER'S NOTE — Claude, 2026-08-16. Read before acting on anything below.
>
> This report was produced by a **read-only Codex pass (`gpt-5.6-sol`, high effort)**. It is a
> *lead list*, not a verified defect list. Every item is labelled "Verified by reading" **by its own
> author**; that label is the author's claim, not an independent check.
>
> **What I checked myself, and what came of it:**
>
> | finding | my verdict |
> |---|---|
> | **VF-05** (radius envelope in `BuildChunkCache`) | ✅ **CONFIRMED and FIXED** — `CODEX-TASK-006`. Real, and the worst of three instances of this class: it is `TunnelNetwork`, it is in code **both** density paths share, and it breaks window invariance (`ARCHITECTURE §8.4`). Genuinely good find. |
> | **VF-02** (3-second shutdown timeout) | ✅ **premise confirmed** — `VoxelWorld.cpp:327` literally reads *"Timeout after 3 seconds to avoid hanging the editor."* Note `CLAUDE.md` states the invariant more strongly than the code implements it ("EndPlay blocks on `ActiveTaskCount → 0`" — it blocks *with a deadline*). Worth deciding deliberately. |
> | **VF-03** (TLS caches omit the owning world) | ⚠️ **substance plausible, EVIDENCE OVERSTATED.** It claims *"the test fixture explicitly documents observed cross-world contamination."* It does not. `VoxelForgeTestFixture.h` documents that the `thread_local` caches exist and flags an unrelated `TSoftObjectPtr` risk. The underlying point (caches keyed on chunk/seed/layout but not on which generator owns them) may still hold — but it needs checking on its own merits, not on this citation. |
> | VF-01, VF-04, VF-06 … VF-10 | **NOT independently verified.** Read them as leads. |
>
> **Do not treat an unverified row as actionable.** The lesson this project keeps paying for is that a
> confident chain resting on an unchecked premise reverses about half the time — and VF-03 is an
> instance of exactly that, inside an audit written to find them.

# VoxelForge code quality and efficiency audit — August 2026

| Finding | File | Severity | Tier | Evidence status |
|---|---|---:|---|---|
| VF-01 — Live rebuilds mutate generation state while workers read it | `VoxelWorld.cpp`, `VoxelStrateManager.cpp` | Critical | Async lifecycle / per-tile workers | Verified by reading |
| VF-02 — Shutdown timeouts allow tasks to outlive their owners | `VoxelWorld.cpp`, `VoxelContentManager.cpp` | Critical | Async lifecycle | Verified by reading |
| VF-03 — Function-static TLS caches omit the owning world/generator | `VoxelGenerator.cpp`, `VoxelStrateManager.cpp`, `VoxelDensityOpStack.cpp` | Critical | Per-voxel caches; per-chunk refill | Verified by reading |
| VF-04 — Box/capsule edits bypass the intended budget and use the wrong live-deco removal volume | `VoxelDiffLayer.cpp`, `VoxelDiffLayer.h`, `VoxelWorld.cpp` | High | Per modification | Verified by reading |
| VF-05 — Cave collection bounds can be smaller than generated geometry when min/max fields are reversed | `VoxelCaveMorphology.cpp`, `VoxelStrateTypes.h` | High | Per-chunk cache construction / skip bound | Verified by reading |
| VF-06 — A fixed-only strate configuration silently disables the strate system | `VoxelWorld.cpp`, `VoxelStrateManager.cpp` | High | Initialization | Verified by reading |
| VF-07 — World origin is used as the “no player” sentinel | `VoxelWorld.cpp`, `VoxelWorld.h` | Medium | Per frame / streaming gate | Verified by reading |
| VF-08 — Decoration palettes are rebuilt every tick and deep-copied into every cell task | `VoxelContentManager.cpp`, `VoxelContentManager.h`, `VoxelStrateTypes.h` | Medium | Per frame and per decoration cell | Verified by reading |
| VF-09 — Clearing decoration builds forgets still-running tasks and defeats the concurrency cap | `VoxelContentManager.cpp` | Medium | Per rebuild / async scheduling | Verified by reading |
| VF-10 — Per-room terrain params are reconstructed for every near-surface sample | `VoxelGenerator.cpp`, `VoxelDensityOpStack.cpp`, `VoxelCaveMorphology.cpp` | Medium | Per near-surface voxel | Verified by reading |

## Scope and evidence

This was a static, read-only review. I read `CODEMAP.md`, `ARCHITECTURE.md` including §8.10, `REVIEW_FINDINGS.md`, the relevant public contracts, implementations, and tests. I did not build, compile, or run the plugin. Every item below is therefore marked **Verified by reading**: the cited control flow or cache-key omission is present in the source. Runtime frequency and timing impact are reasoned from that source, not measured in this review. No “suspicious only” item is included.

The deliberate old/new density-path duplication and every settled decision listed in the review request are excluded.

## Findings

### VF-01 — Live rebuilds mutate generation state while workers read it

**Evidence status:** Verified by reading.

**Location:** `Source/VoxelForge/Private/VoxelWorld.cpp` — `AVoxelWorld::RebuildStrates` (lines 140–153), `AVoxelWorld::OnObjectModifiedInEditor` (238–316), `AVoxelWorld::ChangeSeed` (2064–2114), `AVoxelWorld::LoadTile` (1345–1478), and `AVoxelWorld::GenerateTileResult` (1481 onward); `Source/VoxelForge/Private/VoxelStrateManager.cpp` — `UVoxelStrateManager::Initialize` (28–163).

**What is wrong:** chunk tasks capture `this` and call `GenerateTileResult`, which reads `Generator`, `Mesher`, and through them `StrateManager`. Meanwhile, each live-rebuild path mutates the same objects on the game thread. `Initialize` empties and repopulates `StrateLayout`, empties and repopulates `Passages`, and changes cached seed/settings fields. `ChangeSeed` also writes the generator's plain `Seed`/`OriginSpineRadius`. There is no lock, immutable snapshot, or worker quiescence around those writes.

The order makes the race especially direct: `RebuildStrates` and `OnObjectModifiedInEditor` call `StrateManager->Initialize(...)` before `RegenerateAllChunks()` increments `GenerationEpoch`. `ChangeSeed` also changes generator and manager state before regeneration. The epoch only rejects a finished result; it does not make concurrent reads of reallocating `TArray`s safe and cannot repair undefined behavior that happened while producing the result. Decoration and density-volume workers also read the generator and need to be included in the same transition.

**Why it matters:** an edit or seed change during active streaming can race a worker iterating or indexing storage that `Initialize` has freed/reallocated. Outcomes range from a tile built from mixed old/new settings to an access violation. This is a correctness and lifetime issue, not merely stale-result work.

**Concrete change:** introduce an immutable generation snapshot containing the seed, layout, passages, resolved definitions/op data, and a unique generation ID. Atomically publish the new snapshot and have every task capture a strong reference to one snapshot. The smaller alternative is a rebuild barrier: stop new chunk/deco/density work, wait without timeout for all generator readers, mutate the state, bump the epoch, then resume. Incrementing the epoch before mutation is useful but is not sufficient without snapshotting or quiescence.

### VF-02 — Shutdown timeouts allow tasks to outlive their owners

**Evidence status:** Verified by reading.

**Location:** `Source/VoxelForge/Private/VoxelWorld.cpp` — `AVoxelWorld::EndPlay` (320–375) and the `[this, ...]` task in `AVoxelWorld::LoadTile` (1456–1478); `Source/VoxelForge/Private/VoxelContentManager.cpp` — `UVoxelContentManager::BeginDestroy` (58–63), `NotifyShutdown` (65–80), and the `[this, ...]` task in `LaunchDecoTasks` (384–404).

**What is wrong:** both shutdown drains stop waiting after three seconds and continue teardown while work may still be running. Chunk tasks retain a raw `this` and even their RAII guard holds a reference to `AVoxelWorld::ActiveTaskCount`. Decoration tasks retain a raw content-manager `this`, read its `bShuttingDown`, and may access its queue. `BeginDestroy` sets a flag but does not establish task completion before calling `Super::BeginDestroy`.

The decoration counter is also file-global (`GActiveDecoTasks`), so it is neither an ownership handle nor a per-manager proof that this manager's tasks are finished.

**Why it matters:** if the timeout is reached, later task reads, queue writes, or the guard decrement can target an object whose EndPlay/destruction has advanced. The timeout converts a slow task into a possible use-after-free. The shutdown flag reduces ordinary latency but does not cancel a task already inside generation or marching.

**Concrete change:** retain `UE::Tasks::FTask` handles per owner and make UObject destruction contingent on their completion. Stop submissions first, request cancellation, and either wait unconditionally in a safe shutdown phase or defer final destruction through `IsReadyForFinishDestroy` until the owner's task group is empty. Replace the global decoration count with per-instance task ownership. A watchdog may log a long wait, but it must not release the objects that unfinished tasks can still touch.

### VF-03 — Function-static TLS caches omit the owning world/generator

**Evidence status:** Verified by reading. The test fixture explicitly documents observed cross-world contamination.

**Location:**

- `Source/VoxelForge/Private/VoxelGenerator.cpp` — `UVoxelGenerator::GetDensityAt`: `CP_*` cache (583–622), `GSurfColCache` access (738), and `DiffSlots` (808–826); `ClassifyTile`: `TC_BiomeCache`/`TC_SeenVersion` (2720–2731); `GetBiomeMaterialAt`: `BM_*` cache (3470–3485).
- `Source/VoxelForge/Private/VoxelStrateManager.cpp` — `GeneratePassages` (169–173, 352–353) and `EvaluateModifierSDF`: `SL_*` shortlist plus unchecked `Passages[PIdx]` (372–420).
- `Source/VoxelForge/Private/VoxelDensityOpStack.cpp` — `FRoomGraphSource::Eval`: `SI_*` strate-index memo (2172–2188).
- `Source/VoxelForge/Private/Tests/VoxelForgeTestFixture.h` — `FTestWorld` construction (126–152).

**What is wrong:** these are function/file-static `thread_local` caches, so one worker-thread cache is shared by every VoxelForge instance evaluated on that thread. Their keys use coordinates and per-instance counters such as `LayoutVersion` or `ModsVersion`, but omit the owning generator/manager/diff layer. Two freshly initialized worlds normally both report layout version 1; two diff layers also start with the same modification version. Equal coordinates and versions therefore make the second world reuse the first world's params, operator stack (including its manager pointer), biome context, surface columns, or modification snapshot.

This is not hypothetical test hygiene. `VoxelForgeTestFixture.h` states that two test worlds both reporting version 1 caused the second world to receive the first world's params and `CP_UseOpStack`; it works around the problem by repeatedly initializing each test manager until its version is process-unique. Production has no such workaround.

The passage cache has a more severe failure mode. `SL_Nearby` stores indices from manager A, then manager B with the same `(chunk, PassagesVersion)` can execute `Passages[PIdx]` without `IsValidIndex`. Also, `GeneratePassages` empties `Passages` and returns for an empty layout before incrementing `PassagesVersion`, so the same manager can retain stale indices after an empty rebuild.

**Why it matters:** multiple VoxelWorld actors, PIE worlds, tests, previews, or address-reused objects can produce density/materials/modifications from the wrong world. The passage case can read out of bounds. This affects the per-voxel tier—up to roughly 35³ = 42,875 base samples per full-resolution tile—although the bad selection occurs at cache-refill granularity.

**Concrete change:** give each immutable generation context a process-unique, monotonic cache ID and include it in every shared TLS key. Give each diff layer its own unique ID as well. Prefer a per-worker cache object scoped to that context over scattered function statics. Move the passage-version increment so every clear/rebuild, including the empty-layout exit, invalidates the cache; retain `Passages.IsValidIndex(PIdx)` as defense in depth. Remove the test fixture's serial-bump workaround once production keys express owner identity.

### VF-04 — Box/capsule edits bypass the intended budget and use the wrong live-deco removal volume

**Evidence status:** Verified by reading.

**Location:** `Source/VoxelForge/Private/VoxelDiffLayer.cpp` — `UVoxelDiffLayer::CanModify` (20–45) and `ApplyModification` (63–133); `Source/VoxelForge/Public/VoxelDiffLayer.h` — `FVoxelModification::GetWorldBounds` (97–121); `Source/VoxelForge/Private/VoxelWorld.cpp` — `AVoxelWorld::ApplyModification` (1839–1871), `CarveBox`/`FillBox` (1874–1893), and `CarveCapsule`/`FillCapsule` (1896–1915).

**What is wrong:** budget validation knows only a scalar radius and always charges `4/3*pi*r^3`. `ApplyModification` clamps `Mod.Radius`, but leaves `BoxExtent`, `CapsuleEnd`, and `Falloff` unchanged. The shape-aware AABB then uses those unchanged values. Consequently:

- a box with huge extents is stored across its full huge AABB even though only its proxy radius was clamped and sphere volume was charged;
- a capsule of arbitrary length is charged only as a sphere of its tube radius;
- an untrusted or accidental large shape can enumerate and allocate entries for an enormous number of chunks despite `MaxBrushRadius`/`MaxTotalVolume` being presented as safety limits.

The live decoration cleanup is inconsistent in the other direction. It always removes a sphere centered at `Modification.Center` with the original `Modification.Radius`. For a capsule this is only endpoint A, leaving decorations floating along most of the segment. For a box, `max(half extent)` does not cover the corners and ignores falloff. It also does not use the clamped modification that was actually stored.

**Why it matters:** the budget can be bypassed precisely by the shapes most able to create a large remesh/storage burst. Separately, box/capsule edits leave visibly invalid live content until a later decoration rebuild.

**Concrete change:** make validation and accounting accept the complete `FVoxelModification`. Validate finite, non-negative geometry; enforce extent/tube-radius and capsule-length limits; and charge a documented shape volume (or a deliberately conservative support-AABB volume including falloff). Return the normalized/applied modification or its actual bounds from `ApplyModification`. Use those applied bounds for decoration invalidation—prefer a shape-aware removal query, or at least a conservative sphere centered on the bounds center with the bounds half-diagonal. Keep `CanModify` and `ApplyModification` on the same normalization/accounting function so UI/server decisions cannot drift.

### VF-05 — Cave collection bounds can be smaller than generated geometry when min/max fields are reversed

**Evidence status:** Verified by reading.

**Location:** `Source/VoxelForge/Private/VoxelCaveMorphology.cpp` — `VoxelCaveMorphology::BuildChunkCache`: `MaxInfluence` (127–130), `CollectMargin` (152), `RoomZBuffer` (171), room radius generation (260–265), and tunnel radius generation (464–468); `EvaluateSDF` margin (871–874). Authoring fields are in `Source/VoxelForge/Public/VoxelStrateTypes.h` — `MinRoomRadius`/`MaxRoomRadius` (490/498) and `TunnelMinRadius`/`TunnelMaxRadius` (595/602).

**What is wrong:** `FMath::Lerp(Min, Max, t)` produces values up to `max(Min, Max)` even when an asset has the endpoints reversed. The collection/influence math assumes the field named `Max*` is numerically largest: it uses only `MaxRoomRadius` and `TunnelMaxRadius`. The properties have no cross-field validation enforcing `Min <= Max`.

If `MinRoomRadius > MaxRoomRadius`, actual generated rooms can be larger than `MaxInfluence`, `CollectMargin`, and `RoomZBuffer` assume. If `TunnelMinRadius > TunnelMaxRadius`, the same applies to tunnel reach. `RoomReachesSearchBox` uses the actual radius, but it cannot test a room whose anchor cell was never collected because the collect region was too small. The convenience wrapper repeats the underestimated margin.

**Why it matters:** this is an under-bound, not a conservative overestimate. A room/tunnel able to affect a chunk may not be created in that chunk's cache, producing window-dependent density, seams, missing mesh, or missing collision. Reversed ranges are authorable and can also arise transiently while live-editing the two fields.

**Concrete change:** derive bound-only envelopes as `Max(MinRoomRadius, MaxRoomRadius)` and `Max(TunnelMinRadius, TunnelMaxRadius)` and use them in `MaxInfluence`, collection margins, vertical room buffer, and the wrapper margin. Do not reorder the endpoints passed to `Lerp`, because that would change deterministic room/tunnel assignment; only make the bounds cover every value the existing interpolation can produce. Add asset validation that warns on reversed or non-positive ranges.

### VF-06 — A fixed-only strate configuration silently disables the strate system

**Evidence status:** Verified by reading.

**Location:** `Source/VoxelForge/Private/VoxelWorld.cpp` — `AVoxelWorld::BeginPlay` (413–421); `Source/VoxelForge/Private/VoxelStrateManager.cpp` — `UVoxelStrateManager::Initialize` (43–55, 76–105).

**What is wrong:** `BeginPlay` creates the strate manager only when `Settings->StratePool.Num() > 0`. The manager itself explicitly supports fixed entries independently: it loads `FixedStrates` and selects a fixed definition before consulting the shuffled pool. A valid setup in which every requested slot is fixed and `StratePool` is empty therefore never constructs the manager.

**Why it matters:** the generator silently falls back to generic TunnelNetwork terrain, while content and atmosphere receive a null manager. Authored fixed strata are ignored without an initialization error.

**Concrete change:** initialize the manager when either `StratePool` or `FixedStrates` is non-empty. Validate that every index in `[0, TotalStrates)` can resolve either a fixed definition or a pool fallback, and emit a clear error for uncovered slots rather than silently changing generation mode.

### VF-07 — World origin is used as the “no player” sentinel

**Evidence status:** Verified by reading.

**Location:** `Source/VoxelForge/Private/VoxelWorld.cpp` — `AVoxelWorld::Tick` (456–484) and `GetPlayerPosition` (529–537); declaration/comment in `Source/VoxelForge/Public/VoxelWorld.h` (634–635).

**What is wrong:** `GetPlayerPosition` returns `FVector::ZeroVector` when there is no pawn, but a pawn at the real world origin returns the same value. `Tick` tests `PlayerLastPos != FVector::ZeroVector` before all terrain streaming, atmosphere, decorations, landmarks, water, and density-volume updates.

**Why it matters:** origin is a common initial spawn. While the pawn is exactly there, no initial terrain/content streaming is submitted; behavior begins only after it moves away.

**Concrete change:** return success separately from the coordinate (`bool TryGetPlayerPosition(FVector& Out)` or an optional), or obtain the controller/pawn in `Tick` and gate on pointer validity. Treat every finite coordinate, including zero, as a valid position.

### VF-08 — Decoration palettes are rebuilt every tick and deep-copied into every cell task

**Evidence status:** Verified by reading.

**Location:** `Source/VoxelForge/Private/VoxelContentManager.cpp` — `UVoxelContentManager::UpdateDecorations` (147–256) and `LaunchDecoTasks` (366–398); `Source/VoxelForge/Public/VoxelContentManager.h` — `FDecoCellResult::Entries` (151–158); nested decoration arrays in `Source/VoxelForge/Public/VoxelStrateTypes.h` — `FDecoCompanion::SubCompanions` (2073–2076) and `FStrateDecoration::Companions` (2120–2123).

**What is wrong:** `UpdateDecorations` is called every tick. Before it checks whether the player changed cell or stratum, it resets both flattened palettes, walks every biome and decoration, and copies every `FStrateDecoration`. Those structs contain nested `TArray`s, so this is not a trivial POD copy. `LaunchDecoTasks` then deep-copies the same grid palette and biome-tag array once for every cell task and moves that copy through the result solely so `EntryIdx` can be decoded on the game thread.

At the default 4x4 region size, one new region is 16 cell tasks carrying 16 copies of the same immutable palette. The per-frame rebuild also contradicts the nearby “cheap no-op unless the player crosses a decoration cell boundary or changes strate” expectation.

**Why it matters:** this creates allocator traffic and memory bandwidth on both the steady game-thread path and every decoration-streaming burst. Large biome palettes with companion/sub-companion trees amplify the cost.

**Concrete change:** build an immutable resolved palette snapshot only when its inputs change (stratum/layout/asset revision, tier assignment, or relevant settings). Capture a thread-safe shared reference in cell tasks and carry that same reference in results, or resolve spawn commands to a compact immutable profile table once. Continue draining tasks/results each tick, but do not destroy and reconstruct unchanged nested arrays.

### VF-09 — Clearing decoration builds forgets still-running tasks and defeats the concurrency cap

**Evidence status:** Verified by reading.

**Location:** `Source/VoxelForge/Private/VoxelContentManager.cpp` — `ResetGridBuildState` (88–94), `LaunchDecoTasks` (344–404), `ProcessDecoResults` (763–774), and `ClearAllDecorations` (983–994).

**What is wrong:** `ClearAllDecorations` abandons builds and calls `ResetGridBuildState`, which clears `InFlightCells` even though the corresponding tasks are not cancelled or awaited. A new build can immediately launch another task for the same cell. When the old result arrives, `ProcessDecoResults` removes `R.Cell` from `InFlightCells` before checking its `BuildId`; this can remove the new task's marker. Payload merging is protected by `BuildId`, but scheduling ownership is not.

The throttle uses `NearGrid.InFlightCells.Num() + FarGrid.InFlightCells.Num()`, not `GActiveDecoTasks`, so forgotten/incorrectly removed markers allow actual worker count to exceed `MaxConcurrentDecorationTasks`. Repeated clear/rebuild cycles can compound the excess precisely when live editing or regeneration is already generating other work.

**Why it matters:** the configured cap is documented as preventing decoration marching from crowding mesh-generation workers. This bookkeeping path invalidates that guarantee and can create avoidable CPU/memory bursts. It can also cause redundant same-cell work, though `BuildId` prevents duplicate applied decorations.

**Concrete change:** track an in-flight token that includes grid, cell, and build ID (for example, `TMap<FIntPoint, uint32>`), and remove it only when the completing result owns that exact token. Do not erase live tokens when abandoning build payloads; retain them until completion/cancellation. Better, keep per-instance task handles/counts and throttle on the actual running count, with build identity used only for result relevance.

### VF-10 — Per-room terrain params are reconstructed for every near-surface sample

**Evidence status:** Verified by reading.

**Location:** `Source/VoxelForge/Private/VoxelGenerator.cpp` — `UVoxelGenerator::GetDensityWithParams`, per-room block (1294–1328); `Source/VoxelForge/Private/VoxelDensityOpStack.cpp` — `FRoomGraphSource::FState`/`LocalParams` (1993–2012, 2041–2073); `Source/VoxelForge/Private/VoxelCaveMorphology.cpp` — room-op selection and existing per-room feature pre-bake (653–678).

**What is wrong:** for every `bNearCaveSurface` sample, the original path copies the roughly 74-field `FStrateGenerationParams` and makes a virtual `RoomOp->ApplyTo` call for the nearest room. The operator stack preserves one such copy per sample through `LocalParams()`—correctly memoized so eleven detail operators do not each repeat it—but the work is still invariant for all samples whose nearest cached room is the same.

`BuildChunkCache` already selects `RoomOp`/weight per cached room and calls `ApplyTo` once per room to pre-bake pits, chimneys, and columns. The remaining detail-op parameters can be resolved at that same per-room tier.

**Why it matters:** this is inside the density hot path. A full-resolution tile has about 42,875 base grid samples, plus density calls used for surface normals; only near-surface samples pay this block, but those are exactly the samples concentrated around generated geometry. Copying a large struct and dispatching virtually per sample adds bandwidth and instruction cost that could be per-room/per-chunk.

**Concrete change:** add a compact `FResolvedRoomDetailParams` to `FCachedRoom`, containing only fields consumed by the eleven per-room detail stages, and populate it once during `BuildChunkCache` from the chunk's base params plus the selected op. Both the oracle path and operator-stack path should reference that shared resolved payload. Keep the existing operation order and verify bit-for-bit equivalence; this is a hoist of loop-invariant data, not a split or transcription of `BuildChunkCache`.

## Priority order

Fix VF-01 through VF-03 first: they are memory-model/lifetime/cache-identity problems and can produce crashes or cross-world corruption. VF-04 through VF-06 are deterministic correctness failures with bounded, local fixes. VF-07 is a small but user-visible initialization defect. VF-08 through VF-10 are worthwhile efficiency changes after the correctness hazards are closed; VF-08 and VF-09 should be addressed together because an immutable palette snapshot and explicit task ownership naturally simplify both paths.
