# VoxelForge investigation — T1.d and SurfaceWorld column memo

Date: 2026-08-16  
Base: `experimental` at `4ba53f2`  
Method: static source review only. **No build, compile, editor launch, or automation test was run.**

## Executive conclusion

The current explanation “one unticked neighbouring strate makes the exhaustive per-chunk sweep
reject otherwise-safe boundary tiles” is not what the code does.

`UVoxelStrateManager::UsesOperatorStackForChunk` depends only on `ChunkCoord.Z`. Before the full XYZ
sweep, `UVoxelGenerator::ClassifyTile` has already visited every sampled lattice Z, required the
operator-stack predicate for every cave Z it sampled, and required all cave samples to belong to one
layout slot. Within that one slot the flag cannot vary with X or Y. The later XYZ flag check is
therefore redundant with the current layout implementation.

That does **not** make cross-strate boundary tiles safe to classify with one stack. They still fail
the independent one-slot, generator-type, and bit-identical-params guards. Removing only the later
flag check would not unlock those tiles. Requiring the density samples represented by one verdict to
use one implementation is necessary in principle; the current exhaustive re-check is over-strict/
redundant, but it is not the measured T1.d blocker.

The diagnostic is less precise than the log claims: the initial `UsesOperatorStackForChunk` check
runs before the different-slot check, so a coarse/boundary tile that reaches an unticked adjacent
cave slot can increment `Cave Bail Not Op Stack` even though it would subsequently have failed as
mixed content. The measured 80% therefore does not distinguish “the flown slot itself is unticked”
from “a boundary tile encountered an unticked slot first.” The asset state still has to be read in
the editor.

For the column memo, the port is genuinely incomplete. `GSurfColCache` is a six-box spatial LRU;
`FSurfaceColumnSource::GetColumn` currently owns one 81x81 direct-indexed box. A recenter or
`ColumnKey` change clears all 6,561 computed flags in that one box. This is a verified structural
difference, but the observed 10–30% location-dependent miss band does not prove its performance
impact. Only the same seed and same route can do that.

## Findings

| Status | Finding | Evidence and consequence |
|---|---|---|
| **Verified by reading** | The late “every chunk opted in” sweep is redundant today. | `VoxelGenerator.cpp`, `UVoxelGenerator::ClassifyTile`; `VoxelStrateManager.cpp`, `UVoxelStrateManager::UsesOperatorStackForChunk`. The predicate ignores X/Y and is constant for a layout slot. The earlier Z loop and `CaveBotChunkZ` guard already establish one opted-in cave slot. |
| **Verified by reading** | Boundary tiles remain unsafe for a one-stack verdict for reasons independent of the flag. | `ClassifyTile` rejects a second cave slot, a different generator type, and any non-bit-identical parameter struct. Gradient/Interleaved transitions can vary params per chunk. Removing the redundant flag re-check alone cannot change the safe result from `Mixed`. |
| **Verified by reading** | `Cave Bail Not Op Stack` is an ambiguous attribution counter. | The first predicate check occurs before `GetStrateChunkZBounds`/different-slot attribution. A boundary tile can be counted as NotOpStack even though mixed content would also reject it. Terrain correctness is unaffected; diagnosis is affected. |
| **Suspicious, needs checking** | Most of the measured 1.42/1.77 NotOpStack bails come from the primary cave asset itself being unticked. | This is the simplest explanation for interior tiles, but `.uasset` state is not readable from this source tree and the counter does not separate primary-slot from boundary-slot failures. Confirm in the editor or use Approach A's initialization log. |
| **Verified by reading** | The op-stack column memo loses an entire 6,561-cell working set on any recenter/key change. | `VoxelDensityOpStack.cpp`, `FSurfaceColumnSource::GetColumn`, has one direct-indexed box. `VoxelGenerator.cpp`, `FSurfaceColumnCache`, has `NumBoxes = 6` and evicts only one LRU box. |
| **Suspicious, needs checking** | The one-box design materially causes the observed 10–30% in-game miss rate. | Plausible and location-sensitive, but unmeasured. The retracted different-route comparison cannot support a before/after claim. |
| **Verified by reading** | VF-03's core CP-cache contamination defect is present, and the current source now contradicts the audit reviewer's header. | `VoxelGenerator.cpp`, `UVoxelGenerator::GetDensityAt`, keys the function-static `thread_local CP_*` state by `(ChunkCoord, LayoutVersion)` with no generator/world identity. Each manager's version begins at the same value. `VoxelForgeTestFixture.h`, `FTestWorld::Build`, now explicitly documents the contamination and repeatedly calls `Initialize` to give test worlds process-unique versions. Production has no equivalent owner key. A second world on the same worker can reuse the first world's params, `CP_UseOpStack`, and stack. |
| **Suspicious, needs checking** | Every other TLS cache named in audit VF-03 has the same cross-world exposure. | Several keys visibly omit an owner, but this pass proved the `CP_*` path only. The broader `OC_*`, `BM_*`, passage shortlist, biome, diff, and op-local cache set should be audited as one owner-identity task rather than assumed from the old audit row. Both approaches below leave this open. |

The audit header says VF-03's fixture citation was fabricated. That statement is stale relative to
the checked-out source: the cited explanatory block exists now in `VoxelForgeTestFixture.h` and is
specific about `CP_UseOpStack` contamination. This does not automatically validate every cache
listed in VF-03; it does validate the CP-cache defect above.

## Two implemented approaches

| | Approach A — explicit opt-in + six-box spatial LRU | Approach B — code-enforced cutover + four-way associative memo |
|---|---|---|
| Strategy | Preserve the per-asset A/B contract. Add initialization-time warnings naming every disabled layout slot; the human fixes the `.uasset`. Port the reference six-box LRU to the op source. | Ignore the serialized flag at runtime and route all eight ported archetypes through the stack. Keep the UPROPERTY for asset compatibility. Replace the box with 4,096 exact coordinate entries arranged as 1,024 sets x 4 ways. |
| T1.d effect | No silent behavior change. T1.d fires only after the relevant assets are enabled. Boundary tiles still conservatively fail for slot/params reasons. | `Cave Bail Not Op Stack` should disappear for every valid ported slot without asset edits. `GetDensityAt` and `ClassifyTile` still use the same manager predicate and same stack factory. |
| Cache behavior | Six independent 81x81 boxes. An acquisition miss recenters/clears one LRU victim; five boxes remain warm. Closest match to the proven reference. | Hash routes by exact X/Y plus both halves of `ColumnKey`; exact X/Y/key comparison decides hits. A miss evicts one entry in one set. No bulk clear. Four-way conflicts and capacity eviction remain possible. |
| Runtime/memory cost | Approximately 0.79 MiB TLS per worker for this memo, versus roughly 0.13 MiB for the current single box. Six linear box checks per integer query. | Approximately 160 KiB TLS per worker (compiler padding can change this). Four exact probes per integer query plus hashing/LRU-rank updates. |
| Main risk | High per-worker memory multiplication. The diagnostic currently enumerates SurfaceWorld slots too, although SurfaceWorld's exact-lattice T1.d path does not depend on this flag; narrow that warning before adopting it. | Intentional behavior change for every false-flag asset; the switch can no longer be selected for production A/B. Existing false/true ClassifyTile fixtures now exercise the same runtime path, reducing the distinction between those two end-to-end tests. Associative conflicts may underperform the spatial LRU. |
| What it does not solve | VF-03 owner identity; boundary params/slot conservatism; proof of memo benefit. | VF-03 owner identity; boundary params/slot conservatism; proof of memo benefit. |

### Recommendation

Start from **Approach A**, after narrowing its warning to cave archetypes or rewording the
SurfaceWorld entry. The code does not justify weakening `ClassifyTile`, and the current flag is still
valuable for a same-route A/B. Approach A preserves that measurement lever and ports the known
reference cache design. Its memory cost is the reason not to merge it blindly: measure it against
Approach B on the identical route.

Approach B is the cleaner long-term endpoint only if the project has deliberately decided to retire
the switch as a runtime fallback. It removes configuration drift and is much smaller in TLS, but it
spends the A/B lever and moves every false-flag asset at once. The eight equivalence suites make that
a defensible experiment, not a zero-risk migration.

## Worktrees and files touched

### Approach A

Worktree: `E:\Projet Unreal\VoxelM\Plugins\VF-approach-A`

- `Source/VoxelForge/Private/VoxelDensityOpStack.cpp`
- `Source/VoxelForge/Private/VoxelStrateManager.cpp`
- `CODEMAP.md`

Likely compile-error/watch spots:

- The function-local `thread_local FColumnCache` containing six large aggregate boxes on MSVC/UE's
  TLS implementation.
- Nested local cache types and `FMemory::Memzero` of each victim's `Computed` array.
- The new `UE_LOG` format strings/arguments in `UVoxelStrateManager::Initialize`.

### Approach B

Worktree: `E:\Projet Unreal\VoxelM\Plugins\VF-approach-B`

- `Source/VoxelForge/Private/VoxelDensityOpStack.cpp`
- `Source/VoxelForge/Private/VoxelGenerator.cpp` (comments only)
- `Source/VoxelForge/Private/VoxelStrateManager.cpp`
- `Source/VoxelForge/Public/VoxelDensityOpStack.h` (comments only)
- `Source/VoxelForge/Public/VoxelStrateDefinition.h` (UPROPERTY retained; comments only)
- `Source/VoxelForge/Public/VoxelStrateManager.h` (comments only)
- `Source/VoxelForge/Private/Tests/VoxelForgeClassifyTileTest.cpp` (comments/messages only)
- `Source/VoxelForge/Private/Tests/VoxelForgeTestFixture.h` (comments only)
- `CODEMAP.md`

Likely compile-error/watch spots:

- The function-local `thread_local FColumnMemo` 4,096-entry aggregate.
- `VoxelHash::Mix` calls and casts for X/Y plus low/high halves of the 64-bit key.
- The `uint8` four-way LRU ranks and local-entry default initialization.
- UHT should see no serialized layout change: `bUseOperatorStack` was not removed or renamed.

Both worktrees are detached from `experimental`, contain uncommitted changes, and passed
`git diff --check`. Neither was built or tested.

## Human measurement protocol

Do not compare screenshots or stat averages from different flights. For every baseline/candidate:

1. Use the **same world seed, same asset values, same start point, same route, same speed, same LOD/
   streaming settings, same capture duration, and the same warm-up policy**.
2. Record at least one repeat of the route; scheduler variation can move worker-local cache reuse even
   when the geographic route is identical.
3. Capture `stat VoxelForge` and an Insights trace together. Normalize column misses and worker time by
   `Tiles Meshed`; totals alone conflate cheaper tiles with fewer tiles.
4. Change only one worktree/approach at a time. Do not compare Approach A after an asset edit with
   Approach B before that edit and call it a cache result.

For T1.d, record:

- `Cave Bail Not Op Stack`, `Cave Bail Mixed Content`, `Cave Bail Params`,
  `Cave Bail Stack Verdict`, `Cave Bail Disturbance`, `Cave Bail No Stack`;
- `Tiles Classified`, `Tiles Meshed`, `Tiles Operator Stack Solid/Air`, and total skipped Solid/Air;
- the route segment's active strate and whether each neighboring asset's legacy flag is enabled.

Expected interpretation:

- Approach A: the initialization log must identify any false flag. After the human enables the
  relevant cave definitions, NotOpStack should approach zero in single-strate interiors and
  `Tiles Operator Stack Solid/Air` should become non-zero. Boundary tiles may move to MixedContent,
  Params, or StackVerdict; that is conservative and expected.
- Approach B: NotOpStack should be zero for valid ported cave slots without asset edits. A non-zero
  value then points to out-of-layout/invalid-slot logic or a stale build, not the legacy flag.
- In both: `Tiles Meshed < Tiles Classified` is the production prize. A zero `violations` result and
  all eight bit-equivalence suites green remain mandatory before trusting it.

For the memo, record:

- `Column Memo Hits` and `Column Memo Misses` as a miss percentage;
- misses per `Tiles Meshed`;
- Insights `VoxelForge_ClassifyTile` and `VoxelForge_GenerateMesh` count and time per meshed tile;
- visible LOD-ring update time/throughput on the identical route;
- process memory if comparing the six-box LRU against the associative table at the same worker count.

A lower miss percentage on a different route is not evidence. A valid claim is: same seed, same
route, same settings, same denominator, with the candidate as the only change.

## Ready-to-build status

Both alternatives are ready for the human's review/build step. No result in this document is a
compile or runtime claim; all implementation conclusions are from source and diff inspection.
