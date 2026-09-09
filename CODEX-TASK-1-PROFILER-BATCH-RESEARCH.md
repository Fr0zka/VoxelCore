# VoxelForge Task 1: profiler, batch explorer, and research handoff

Research date: 2026-09-09. This note records the implementation and the read-only web research for Task 1. No RealtimeMeshComponent files were changed.

## 1. Measurement result

The old result in the task brief was 14.74 us/sample on the profiled density path versus 1.28 us/voxel without instrumentation: about 11.5x apparent overhead. That is consistent with putting `FPlatformTime::Cycles64()` around a roughly 100 ns operation. The timer call is not free, and at that granularity the measurement is mostly the measurement.

The new default is sampled profiling:

- Coarse calls and counters stay cheap and are collected in thread-local state.
- `GetDensityAt` uses a deterministic count interval of 64 calls and one amortized block timer, rather than one timer pair per density call.
- Full cycle timing remains an explicit opt-in (`-profiledensityfull`) for a small investigation, not the normal batch mode.
- The default sampled mode reports the parent density/mesher wall estimate. Fine per-operation scopes are disabled in that mode; use Full or Unreal Insights for a detailed phase breakdown.
- Sampling and timers only write report counters. They do not choose cells, alter density values, alter task partitioning, or participate in any reduction, so they cannot affect generated geometry.

The timer calibration in the validation run measured one `Cycles64` pair as approximately one platform cycle, or 0.1 us at the reported clock conversion. That is the direct self-check for the suspected timer-dominates-work failure mode.

Validation on the same 64^3 `TunnelNetwork` export:

| measurement | seconds |
| --- | ---: |
| profiled sampled export | 0.4630500004 |
| fresh unprofiled reference export | 0.5584703982 |
| discrepancy | -17.09% |
| profiled/reference ratio | 0.82914 |

Both exports produced geometry hash `235AC2DC`. The commandlet now puts all four values in every profiled report and fails with exit code 2 when the absolute discrepancy exceeds 20% or the reference hash differs. The report also exposes `cpu_us` (sum of worker CPU estimates) separately from `wall_us`/`total_us` (largest-worker wall estimate). Summing CPU time across 15 workers is not a valid wall-clock parent; collapsing those fields was one way to make a parallel profile lie.

The density report has an explicit ledger. In sampled mode, the parent estimate is the measured quantity, fine phase buckets are marked unavailable, and the unattributed remainder makes the ledger sum exactly to the parent. This is honest about what was measured instead of inventing a fine breakdown from disabled timers.

## 2. Batch explorer

The explorer commandlet accepts one JSON file and keeps one editor process alive for every case:

```text
UnrealEditor-Cmd.exe "<HostProject>.uproject" -run=VoxelForgeExplore -batch="E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BatchValidation.json" -out="E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BatchValidationRun2" -trace=cpu,counters
```

`-out` must be an absolute directory below this plugin's `Saved` directory. The commandlet writes one `batch.json` below that directory. The input supports, per case: `id`, `seed`, `archetype`, `slot`, `export_size`, `export_step`/`step`/`lod`, `modes`, `operator_stack`, `profile_density`, `profile_density_full`, `profile_lod`, render/failure-focus settings, and a per-case budget. The parser caps one batch at 256 cases and clamps the total budget to 30 minutes; each case also has a bounded budget.

Before every case, the commandlet resets the density profile and all per-case report state, then applies the case's profile settings. The output contains per case: mesh seconds, microseconds per voxel, triangle count, geometry hash, counters, profile mode, and the case exit code. Invalid or timed-out cases are represented in the same machine-readable output rather than silently disappearing.

The final three-case confirmation completed in 6.59 seconds of commandlet case time after the single process launch:

| case | triangles | mesh seconds | hash | result |
| --- | ---: | ---: | --- | --- |
| repeat_unprofiled | 2,461 | 0.391420 | `235AC2DC` | pass |
| repeat_profiled | 2,461 | 0.515374 | `235AC2DC` | self-check pass |
| maze_lod1 | 5,218 | 0.277830 | `784EBF92` | pass |

The standalone profiled 64^3 case and the `repeat_profiled` case inside the batch both produced `235AC2DC`, 2,461 triangles, 343,000 tunnel lookups, and 554,381 passage evaluations. That is the contamination check: same final build, seed, archetype, slot, export size, step, and modes produced the same geometry and coarse counters alone and in a reused process. The standalone profiled export also matched its fresh unprofiled reference hash.

## 3. Cache footprint answer

The historical 15-worker report that prompted the question was:

```text
capacity_entries=3840 valid_entries=1542 static_bytes=2534400
dynamic_bytes=428627716 total_bytes=431162116
avg_valid_entry_bytes=278703.6 largest_entry_bytes=541344
tunnel_valid=1054 tunnel_dynamic=345243636
roomgraph_valid=488 roomgraph_dynamic=83384080
```

That is 431,162,116 bytes (411.2 MiB) in aggregate and 278,703.6 bytes per valid entry on average (278.7 kB decimal, about 265.7 KiB). The number is not one monolithic 272 kB struct. Each worker owns cache slots, and valid entries contain variable-length arrays.

A representative 128^3 breakdown added by this task was 360,063,124 bytes total:

| member family | tunnel cache | room-graph cache |
| --- | ---: | ---: |
| slot storage | 1,658,880 | 875,520 |
| op stack | 8,264,000 | 0 |
| rooms | 47,876,912 | 31,822,512 |
| room-floor joins | 4,936,512 | 3,278,976 |
| tunnels and control arrays | 5,560,248 | 3,591,584 |
| support-column entries | 7,153,328 | 0 |
| support columns | 30,699,008 | 0 |
| room index bins + candidates | 30,759,720 | 22,700,380 |
| tunnel index bins + candidates | 60,972,480 | 41,583,988 |
| tunnel-world index bins + candidates | 34,635,272 | 23,693,804 |
| dynamic total | 230,857,480 | 126,671,244 |
| static + dynamic total | 232,516,360 | 127,546,764 |

The dominant allocation is the spatial index bin/candidate storage, followed by rooms and support-column arrays. Slot storage is comparatively small. The exact per-entry average moves with the generated case and cache occupancy, which explains why the 128^3 confirmation is not numerically identical to the historical 411 MiB run.

## 4. Research findings

### RealtimeMeshComponent, read-only

At research time the upstream releases page lists v5.3.2 as the latest tagged release: [RMC releases](https://github.com/TriAxis-Games/RealtimeMeshComponent/releases). The disposable build host's copy identifies itself as `VersionName: 5.4`, so the upstream tag number is not a safe linear comparison; this project also carries custom changes. The visible upstream history includes a 5.1 game-thread/render-thread restructuring and a 5.0.5 release mentioning mesh-update and memory-overuse fixes. The public release page does not provide a complete API diff against this custom 5.4 copy, so I am treating those as compatibility signals rather than promising source compatibility. The local copy was not upgraded, diffed destructively, or otherwise modified.

The current upstream guidance is to build an `FRealtimeMeshStreamSet` as a CPU-side value on a worker, then cross to the game thread to create/update the component section group; use a weak object pointer for the handoff. The structure documentation describes a game/render proxy mirror and distinguishes in-place dynamic/compute updates from static proxy recreation. See [updating mesh data](https://triaxis.games/realtime-mesh/docs/component-core/updating-mesh-data/) and [RMC component structure](https://triaxis.games/realtime-mesh/docs/component-core/structure/). The practical candidate is therefore worker-side stream construction plus the smallest possible section/buffer update, not touching a `URealtimeMeshComponent` from a worker. Custom RMC changes make this a high-risk follow-up.

### UE 5.7 tasking and tracing

Epic's UE 5.7 API documentation says `ParallelForWithExistingTaskContext` gives each spawned task a task-local workspace, and `MinBatchSize` limits launches to `DivUp(Num, MinBatchSize)` workers. The pre-work variant runs caller work before that thread helps the parallel loop. Both pages explicitly warn against clogging the task graph with long-running or blocking work: [existing task context](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/ParallelForWithExistingTaskConte-) and [pre-work task context](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/ParallelForWithPreWorkWithTaskCo-). The next tuning target should be measured batch size and task-local scratch reuse for short density/cell jobs, not more tiny tasks.

UE's built-in tracing is the better fine-grained instrument. The documented channels include CPU, Counters, Task, and StackSampling; `-trace=cpu,counters` is a valid launch selection. `TRACE_CPUPROFILER_EVENT_SCOPE` with static scope names and `CountersTrace.h`/`TRACE_COUNTER_*` are the low-cost native hooks. See [Trace channels](https://dev.epicgames.com/documentation/en-us/unreal-engine/trace-in-unreal-engine-5), [the tracing guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/developer-guide-to-tracing-in-unreal-engine?lang=en-US), and the [Unreal Insights reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-insights-reference-in-unreal-engine-5). Timing Insights supplies timers, counters, callers/callees, and task execution views; Task Graph Insights exposes execution paths and critical paths: [Timing Insights](https://dev.epicgames.com/documentation/en-us/unreal-engine/timing-insights-in-unreal-engine?lang=en-US) and [Task Graph Insights](https://dev.epicgames.com/documentation/unreal-engine/task-graph-insights-in-unreal-engine-5?lang=en-US).

This task therefore keeps the lightweight sampled report for automated self-checks and adds native CPU scopes/counters for investigation. Insights should own future fine per-scope and task scheduling analysis; the hand-rolled profiler should not grow back into a timer around every density call.

### Marching cubes and density generation

The upstream [Transvoxel overview](https://transvoxel.org/) confirms that transition cells use local voxel data and a bounded lookup table to stitch different-resolution meshes without cracks. It is relevant to LOD boundary work and dynamic retriangulation, not a reason to change the current mesh in this task. A classic SIMD study reports nearly 4x over an unoptimized serial Marching Cubes implementation and about 2x over compiler-optimized C: [Newman et al., *High performance SIMD marching cubes*](https://www.sciencedirect.com/science/article/abs/pii/S0097849303002760). Those are historical results on older x86 and should be treated as an upper-bound signal, not a VoxelForge forecast.

## 5. Ranked next-task candidates (not implemented here)

The magnitude estimates are expected ranges for the affected stage, inferred from the sources and this run's counters; they are not promises for end-to-end time.

| rank | candidate | expected magnitude | risk |
| ---: | --- | --- | --- |
| 1 | Conservative macrocell/cell empty-space early-out using deterministic density bounds, with a fallback on boundary uncertainty | 2–10x on empty or mostly empty chunks; smaller on surface-heavy chunks | Medium-high: bounds must never skip a crossing cell or change traversal order |
| 2 | Reuse one canonical density grid/field across meshing, LOD, export, and renderer preparation; invalidate by seed/slot/parameters | 20–60% less repeated density work and memory bandwidth where consumers overlap | Medium: cache lifetime and invalidation |
| 3 | Tune `ParallelFor` granularity and task-local scratch for short jobs; measure `MinBatchSize`, worker count, and critical path in Insights | 5–25% less scheduling/coordination cost | Low-medium: load imbalance and contention can reverse the win |
| 4 | SIMD/SoA batches for density and Marching Cubes table work | 1.5–4x for the vectorized stage in favorable workloads | High: exact cross-machine geometry and floating-point contraction; no runtime CPU dispatch is allowed |
| 5 | Hierarchical sparse/occupancy metadata or distance bounds to skip whole regions before field evaluation | 2–20x in sparse worlds | High: build/update cost, conservative topology, and invalidation |
| 6 | RMC partial section/buffer updates after worker-side stream construction | 10–50% less renderer update/bandwidth cost when only part of a section changes | High: custom RMC modifications and game/render-thread ownership |
| 7 | Transvoxel transition cells for LOD boundaries | 10–30% less boundary patch/rebuild work in LOD-heavy scenes; also removes crack workarounds | Medium-high: transition topology and hash stability |

The first safe experiment should be the deterministic early-out or field reuse, after a baseline with the corrected profiler. SIMD is deliberately below those despite its attractive paper result because the project's cross-machine geometry requirement is absolute.

## 6. Pushback and constraints

The requirement that buckets sum to their parent needs a definition in a parallel program. Raw worker CPU time can exceed wall time by the worker count; the report now exposes both and makes the wall estimate the parent used for the self-check. A fine raw-bucket sum cannot be claimed when sampled mode intentionally does not time those scopes, so the report says that explicitly and accounts for the remainder.

The 20% self-check threshold is a practical alarm, not a proof of zero measurement cost: OS scheduling, cache state, DDC state, and editor startup can move two exports. The geometry hash check is exact; repeated cases or a median of several runs should be used when a performance regression is close to the threshold.

The sampling decision is non-deterministic only in its elapsed-time observation. It is isolated from the generation inputs and outputs. The batch commandlet resets profile/cache-report state per case, but it intentionally reuses the engine process to remove startup cost; the standalone-versus-batch geometry hash is the proof that this reuse did not contaminate the tested case.
