# AUDIT-COLD-2026-09-14: read-only examination, with the orchestrator's verification notes

Produced by Codex (gpt-5.6-luna, max effort, read-only sandbox) on 2026-09-14 at HEAD `7a972f5`,
briefed with no project background. It did read the plugin-root docs, so it is not context-free,
but its findings are code-level. Findings are verbatim below. These notes record what the
orchestrator checked against the code afterwards.

## Orchestrator verification notes
- **#7 CONFIRMED, a determinism bug introduced by the tile-cache work (`2de603c`/`cfad3d1`).**
  `VoxelGenerator.cpp:2968` moves `CP_OpStack` into the cache entry and resets `GTunnelCoreCache`.
  A later request for the same `ChunkCoord` under the other `bTileCacheContext` misses the key
  (`:2660`), but the refetch guard (`:2723-2725`) skips because chunk, owner and version are
  unchanged, so evaluation uses the moved-from (empty) stack and core cache. The hit path sets
  `CP_Chunk` to the shared key `(0,0,Z)` (`:2698`), so the spawn column `X=Y=0` reaches this state
  after an ordinary hit, not only in a narrow timing window. Coarse tiles (LOD >= 3) covering the
  origin can be evaluated wrongly depending on the order work reaches a worker.
- **#3 and #8 are real but narrower than stated.** In multiplayer, the server and client worlds share
  seed and layout by design, so the cached values are identical. Contamination needs worlds that
  genuinely differ under the same key: editor edits that do not bump the version, or two diff
  layers with different contents at the same `ModsVersion`. Cheap to fix (add owner/layer identity).
- **#19 is not a defect under C++20** (UE 5.7), where left-shifting negative signed values is
  defined. Overflow for extreme coordinates remains theoretical.
- **#21 matters again** if the gap/seal AllSolid proof (in progress) calls `HasAnyModInChunkRange`
  per LOD0 tile.
- The others (#1, #2, #4-6, #9-18, #20, #22) were not independently re-verified. Verify each before
  fixing it.

---

# Read-only audit report

I made no file changes and did not build or run the plugin. “VERIFIED” means the failure follows directly from the source path; “SUSPECT” means a runtime/compiler-dependent hazard that was not executed.

## Highest impact

1. **The density-volume worker races live generator mutation — P1 — VERIFIED**

   Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp` — `FScopedGenerationPause` (203–222), `AVoxelWorld::RebuildStrates` (551–582), `AVoxelWorld::ChangeSeed` (5569–5623).  
   `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelDensityVolume.cpp` — `UVoxelDensityVolume::Reset` (101–120), `ProcessOneFill` (603–637).

   `FScopedGenerationPause` waits for ordinary generation tasks and decoration tasks, but not the dedicated density-volume fill thread. That thread reads the raw `Generator` pointer and calls `GetDensityAt` while `RebuildStrates` or `ChangeSeed` mutates the generator and strata manager.

   A fill in progress during a PIE edit or seed change can therefore read partially rebuilt arrays or mixed old/new settings. This is an actual data race with crash/garbled-density potential. `Reset` also clears `PendingFills` but not the already queued `FillQueue`, causing stale work to continue after reset.

2. **Decoration tasks can use a destroyed content manager — P1 — VERIFIED**

   Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelContentManager.cpp` — `UVoxelContentManager::BeginDestroy` (58–62), `NotifyShutdown` (65–75), `WaitForDecorationTasks` (78–85), `LaunchDecoTasks` (327–415).

   Tasks capture raw `this` and later access `bShuttingDown` and `DecoResults`. Shutdown waits only three seconds, ignores the timeout result, and `BeginDestroy` immediately calls `Super::BeginDestroy`.

   Any decoration task exceeding the timeout can later enqueue into the destroyed manager. The global task counter also includes tasks belonging to unrelated worlds, so one world can time out because another world is busy.

3. **The diff snapshot TLS cache is not keyed by its owning diff layer — P1 — VERIFIED**

   Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp` — `UVoxelGenerator::GetDensityAt` (3069–3097).  
   `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Public\VoxelDiffLayer.h` — `GetModsVersion` and `ModsVersion` (241, 289).

   `thread_local FDiffSlot DiffSlots[64]` considers only chunk coordinates and `ModsVersion`. It does not include the `UVoxelDiffLayer*`.

   Two PIE worlds commonly have the same chunk and the same per-layer version—for example, both reach version 2 after their first edit. If a worker samples world A first, world B can hit A’s cached modification snapshot and apply the wrong edits. This produces cross-world terrain contamination dependent on worker history.

4. **Fixed-only strata configurations are silently disabled — P1 — VERIFIED**

   Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp` — `AVoxelWorld::BeginPlay` (1294–1305).  
   `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelStrateManager.cpp` — `UVoxelStrateManager::Initialize` (1060–1244).  
   `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp` — fallback generation in `GetDensityAt` (3030–3039).

   `BeginPlay` creates a strata manager only when a season is assigned or `StratePool.Num() > 0`. It does not test `FixedStrates.Num()`.

   A settings asset with valid fixed strata but no season and no pool therefore receives no strata manager. The generator takes its generic fallback path, ignoring the authored fixed definitions.

5. **Settings rebuilds leave dependent systems with inconsistent snapshots — P1/P2 — VERIFIED**

   Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp` — `RebuildStrates` (551–582), `PostEditChangeProperty` (885–895), `OnObjectModifiedInEditor` (898–985).  
   `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp` — `InitializeSettings` (2079–2088).

   `RebuildStrates` reinitializes the strata manager but does not reinitialize generator settings. `PostEditChangeProperty` only regenerates chunks. Asset-change handling recognizes strata and terrain-op definitions, but not general settings or biome assets.

   Changing seed, world radius, edge sealing, the settings object reference, or related configuration can leave the manager using new values while the generator, content manager, or density volume still use old snapshots. `ChangeSeed` has a separate correct reinitialization path, but the other rebuild paths do not. This also contradicts the `RebuildStrates` documentation in `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Public\VoxelWorld.h` (572–575), which promises to re-read all settings.

6. **Invalid strata initialization destroys the previous valid layout — P1/P2 — VERIFIED**

   Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelStrateManager.cpp` — `UVoxelStrateManager::Initialize` (1060–1205).  
   `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp` — `RebuildStrates` (562–576), `OnObjectModifiedInEditor` (975–985).

   `Initialize` empties `StrateLayout`, `SeasonStrates`, and related state before all season/layout validation has succeeded. If an assigned season is missing, malformed, or contains invalid bounds, initialization returns false after the old layout has already been discarded.

   `RebuildStrates` then leaves existing meshes visible while future generation queries see an empty or partial manager. The editor asset-modification path ignores the initialization return value and regenerates anyway, making a transient invalid asset capable of replacing the working world with fallback/empty terrain.

## Material correctness and determinism issues

7. **The operator-stack cache is stale when tile-cache context changes — P2 — VERIFIED**

   Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp` — `UVoxelGenerator::GetDensityAt` (2218–2230, 2255–2261, 2320–2324, 2393–2407, 2551–2573, 2600–2672).

   The tunnel cache key includes whether tile-cache mode is active, but the rebuild guard does not. A fine-LOD request can build and move the operator stack into the cache, leaving the worker-local `CP_OpStack` empty. A later request for the same chunk under a different tile-cache context misses the cache key but does not rebuild the stack because chunk, manager, and layout version are unchanged.

   The empty or mismatched stack is then used for density evaluation. LOD request order can therefore change terrain results.

8. **Several surface and biome TLS caches omit generator/context identity — P2 — VERIFIED**

   Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp` — `FSurfaceColumnCache::Acquire` (274–328), `GetDensityAt` (2692–2705), `GetSurfaceHeightAt` (5149–5166), `ClassifyTile` (6014–6023), `GetBiomeMaterialAt` (7019–7034).  
   `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Public\VoxelBiomeTypes.h` — `FChunkBiomeCache::Contains` (240–245).

   These worker-local caches use combinations of chunk, seed, strata key, and layout version, but not the owning generator, strata manager, or biome context. Two worlds can legitimately share all those scalar keys while having different terrain or biome parameters.

   When a pooled worker handles both worlds, the second world can receive cached surface columns, biome classification, or material data from the first. Direct asset changes that do not advance the relevant version have the same stale-cache problem.

9. **Native shaft/island topology caches omit parameters used to build them — P2 — VERIFIED**

   Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp` — `GetVerticalShaftDensity` (7090–7106, 7211–7219, 7392–7413), `GetFloatingIslandDensity` (7518–7554).

   The shaft cache key omits `LedgeSpacing` and `LedgeDepth`, although both affect connector and ledge placement. The floating-island cache key omits `BoundarySealThickness`, although it affects cached vertical spread.

   Changing these values—or using two contexts with identical keyed values but different omitted values—reuses old topology on the same worker.

10. **Completed pre-edit decoration results can resurrect removed objects — P2 — VERIFIED**

    Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp` — `ApplyModification` (5319–5375).  
    `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelContentManager.cpp` — `LaunchDecoTasks` (381–404), `ProcessDecoResults` (769–807), `MergeCellResult` (815–862), `RemoveDecorationsInSphere` (1249–1260).

    Applying a live diff removes currently applied decorations but does not invalidate queued/in-flight decoration builds or associate them with a diff version. `BuildId` only identifies decoration-region rebuilds.

    Sequence: a task computes old grass/props, the edit removes current instances, then the old result is drained and accepted. The old decorations reappear over the edited terrain until another rebuild occurs.

11. **Landmark conditions are evaluated without the actual biome context — P2 — VERIFIED**

    Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelContentManager.cpp` — `UpdateDecorations` (153–263), `UpdateLandmarks` (1263–1310), `SpawnFromProfile` (1101–1137).  
    `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp` — `EvaluateTerrainConditions` (6682–6717).

    Normal decoration updates populate `FDecoContext::BiomeCtx`; landmark updates construct a context but never set `BiomeCtx`. A landmark profile using biome, moisture, relief, or biome-border conditions therefore evaluates with the default/invalid biome map.

    On a non-default biome map, landmarks can spawn or fail to spawn differently from ordinary decorations.

12. **Decoration cleanup uses local coordinates as world coordinates — P2 — VERIFIED**

    Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp` — `ApplyModification` (5369–5375); `WorldToLocalVoxel` and `LocalVoxelToWorld` (2314–2321).  
    `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelContentManager.cpp` — `RemoveInstancesInContent` (1231–1242).

    `Modification.Center` is a local voxel coordinate, but cleanup computes `Center * VOXEL_SIZE` and passes it to a world-space overlap query. For a translated actor, the query is centered at the wrong location.

    The same cleanup also approximates boxes and capsules with only `Modification.Radius`, which does not cover a long capsule or a box’s full extent. Existing decorations can remain over an edit or unrelated origin decorations can be removed.

13. **Decoration application order is timing-dependent — P2 — VERIFIED**

    Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelContentManager.cpp` — `LaunchDecoTasks` (327–415), `ProcessDecoResults` (769–810), `ApplyRegion` (885–952), `RebuildDesiredCells` (281–324).

    Tasks enqueue results in completion order. Results append actor spawns and HISM transforms in that order; mesh buckets are iterated through a `TMap`. Equal-distance pending regions also have no deterministic tie-breaker.

    The same candidate set can therefore produce different actor spawn order, NetGUID/side-effect order, HISM instance indices, and internal clustering across runs or machines.

14. **The passage shortlist version is not advanced when passages are cleared — P2/P3 — VERIFIED**

    Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelStrateManager.cpp` — `GeneratePassages` (1611–1620, 2477), `EvaluateModifierSDF` (2488–2524), `VF_GetNearbyPassages` (294–338).  
    `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Public\VoxelStrateManager.h` — `GetLayoutVersion` and `PassagesVersion` (250, 515, 519).

    `GeneratePassages` empties `Passages` and returns early for an empty layout without incrementing `PassagesVersion`. Existing worker TLS nearby-passage caches still match the manager lifetime, chunk, and unchanged version.

    After a previously valid layout becomes empty, `EvaluateModifierSDF` can reuse indices into the now-empty array and dereference invalid passage data. This is an edge configuration, but it is reachable through the invalid-layout paths above.

## Input-contract and performance issues

15. **Diff modifications accept nonfinite and invalid geometry — P2/P3 — VERIFIED for malformed input**

    Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelDiffLayer.cpp` — `CanModify` (20–44), `ApplyModification` (63–96), `EvaluateMods` (191–248).  
    `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp` — `CarveAtPosition` (5301–5307).

    There is no finite-value validation for strength, radius, centers, endpoints, or extents. A NaN strength survives `FMath::Abs`, then propagates through `TotalOffset` and density evaluation. Infinite strength produces infinite density. Negative or zero radii can consume budget while producing no meaningful spherical modification.

    This is reachable from C++ or malformed Blueprint/runtime data and permanently poisons the diff layer until cleared.

16. **The public diff API contradicts itself about coordinate units — P2/P3 — VERIFIED**

    Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Public\VoxelDiffLayer.h` — `FVoxelModification::Center` (64), `CapsuleEnd` (86).  
    `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Public\VoxelWorld.h` — `ApplyModification` documentation (334–340).  
    `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelDiffLayer.cpp` — `GetWorldBounds` (88–96).

    The modification struct comments describe the center as world-space, while the low-level world API and implementation interpret it as voxel-space. The helper functions explicitly convert world centimeters into voxel coordinates.

    A caller following the struct comment and supplying centimeters gets an edit displaced by a factor of `VOXEL_SIZE`.

17. **Diff budget accounting underestimates boxes and long capsules — P3 — VERIFIED**

    Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp` — `CarveBox`/`FillBox` (5379–5398), capsule helpers (5401 onward).  
    `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelDiffLayer.cpp` — `CanModify` (34–39).

    The budget charges every shape as a sphere with volume based on `Modification.Radius`. Boxes use the maximum half-extent as a proxy; capsules use the tube radius even when endpoints are very far apart.

    A capsule with radius 3 and endpoints 1000 voxels apart is charged like a tiny sphere despite affecting a long volume and many chunks. Large edits can therefore bypass the intended modification budget and cause excessive remeshing.

18. **Decoration radius/spacing changes are ignored while the player is stationary — P3 — VERIFIED**

    Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelContentManager.cpp` — `UpdateDecorations` (199–256), `RebuildDesiredCells` (265–325).

    Radius and spacing are refreshed every update, but desired-cell rebuilding occurs only when `PlayerCell != LastDecoCell`.

    Shrinking the radius leaves distant regions loaded; expanding it does not enqueue new regions; changing spacing leaves existing regions at the old grid layout. Movement eventually repairs the state, but a stationary player sees stale decoration coverage.

19. **Signed left shifts of negative chunk coordinates are undefined — P3 — SUSPECT**

    Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp` — `BuildDesiredTiles` (3498–3548).

    Streaming computes coordinates with expressions such as `T.Z << L`. Ordinary underground tiles have negative `T.Z`, so this left-shifts a negative signed integer. Large positive coordinates can also overflow.

    The exact manifestation is compiler/optimization-dependent, so I classify this as SUSPECT rather than a fully end-to-end verified runtime failure.

20. **Interleaved transitions can divide by zero — P3 — VERIFIED math path**

    Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Public\VoxelStrateDefinition.h` — `TransitionBlendChunks` (99–101).  
    `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelStrateManager.cpp` — `GetGenerationParams` (5673, 5728–5753, 5826).

    The property metadata clamps the value in the editor, but runtime/cooked data can still contain zero. The interleaved transition path divides warped distance by `EffectiveBlend` without a zero guard.

    A zero-width transition can produce Inf/NaN blend values and invalid generation parameters.

21. **Tile classification scans the entire modification map — P3 — VERIFIED**

    Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp` — `ClassifyTileUncached` (5785–5790).  
    `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelDiffLayer.cpp` — `HasAnyModInChunkRange` (170–188).

    Classification calls a routine that walks every `ChunkMods` key under the read lock instead of using the existing per-chunk grouping to narrow the query.

    After many edits, each tile classification becomes O(number of modified chunks), increasing lock hold time and making edit-heavy worlds disproportionately expensive.

22. **Operator documentation is internally stale — P3 — VERIFIED documentation contradiction**

    Location: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Public\VoxelDensityOp.h` (1–3, 86–92).  
    `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp` — `UVoxelGenerator::GetDensityAt` (2393–2600).

    The header says the density-op system is not wired into `GetDensityAt`, while later comments say all eight operators now have the stack path. The generator does in fact execute the operator stack.

    This is not itself a runtime defect, but it is misleading maintenance documentation around a central generation path.

The most urgent issues are the uncoordinated density-volume thread, decoration-task lifetime, cross-world TLS diff cache, and incomplete settings/layout rebuilds.
