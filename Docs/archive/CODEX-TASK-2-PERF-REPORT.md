# Task 2 performance report

Date: 2026-09-09  
Repository: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge`  
Work began at `experimental` / `e63f016`.

## Measurement rule

All performance numbers below are per-case `run.elapsed_seconds` from the batch report. This is
the wall clock for a real commandlet case through the export or render result; it includes the
shared-density-grid build when enabled. `summary.mesh_seconds` and profiler buckets are included
only as diagnostics, never as the performance claim.

The export comparison is the same `128^3` `TunnelNetwork`, seed `1337`, slot `1`, operator stack,
step `1`, with three cases in one commandlet process on each side. The 17% reporting floor is
therefore met by both run count and the retained density-grid result.

## Part 1 — block early-out

I tested an `8^3` block reject behind conservative fallback logic. On this cave/stack world, every
one of the 4,096 blocks was left uncertain and went through normal sampling:

| batch | runs | end-to-end wall seconds | blocks skipped | skip rate | result |
| --- | ---: | --- | ---: | ---: | --- |
| baseline | 3 | 3.805057, 3.522129, 3.569186 (avg 3.632124) | — | — | reference |
| block reject | 3 | 6.341454, 6.562621, 7.300234 (avg 6.734770) | 0 / 4,096 | 0.00% | **reverted** |

The candidate added classification work without skipping a block (+85.42% wall clock). It also
did not produce a useful supremum for the complete cave stack, so I did not promote a guessed
`max` parameter into a topology decision. `ClassifyTile` remains untouched: its stack, disturbance,
diff-layer, and cave guards are not a safe substitute for a new block proof. A false AllSolid
verdict here would delete a 3D cave, which is not an acceptable trade.

This did not clear the floor and was reverted.

## Part 2 — density-grid reuse

The canonical mesher requests 35^3 samples per 32-cell tile. For 64 tiles that is:

- tile requests: `64 * 35^3 = 2,744,000`;
- unique points in the expanded 128-cell lattice: `131^3 = 2,248,091`;
- duplicate evaluations: `495,909` (18.07%).

The first implementation evaluated the shared lattice as Z planes. It removed all duplicates but
was slower because it destroyed the generator's spatial locality:

| candidate | runs | end-to-end wall seconds | duplicates | result |
| --- | ---: | --- | --- | --- |
| matched control | 3 | 3.668954, 3.633445, 3.655658 (avg 3.652686) | 495,909 → 495,909 | reference |
| global-plane precompute | 3 | 5.349520, 5.352927, 5.510752 (avg 5.404400) | 495,909 → 0 | **reverted** |
| owner-grid precompute | 3 | 1.950207, 1.973673, 2.161305 (avg 2.028395) | 495,909 → 0 | retained candidate |

The retained implementation partitions the shared lattice into 64 disjoint rectangular owner
regions. Each global sample is generated exactly once, then each tile copies its local halo window
from the immutable run-scoped grid. The precompute is deliberately inside the wall-clock timer;
the final three-case repeat measured:

| batch | runs | end-to-end wall seconds | average |
| --- | ---: | --- | ---: |
| final control (`density_grid_reuse=false`) | 3 | 3.659587, 3.619013, 3.582257 | 3.620286 s |
| final reuse (`density_grid_reuse=true`) | 3 | 1.908017, 1.854344, 1.884170 | 1.882177 s |

That is a **48.01% wall-clock reduction / 1.92x speedup**, clearing the 17% floor. The final
reuse runs averaged 1.474271 s for the shared-grid build, and still won end to end. The 128^3
shared lattice is about 8.6 MiB of float storage. Direct explorer runs default to reuse; a batch
case can pass `density_grid_reuse=false` for a matched control.

The run-scoped handoff is confined to the explorer's finite canonical export. The streaming game
path has no one-shot set of neighboring tiles or invalidation boundary, so it was not given a
locking plane cache that could serialize workers or outlive a generation epoch.

## Part 3 — task granularity

The final control observed 15 worker thread IDs and 64 logical tile invocations. Across its three
runs, the measured tile-job averages were:

| metric | average |
| --- | ---: |
| fastest logical tile job | 0.188817 s |
| mean logical tile job | 0.593751 s |
| slowest logical tile job | 0.955788 s |
| sum of tile-job durations | 38.000045 s |
| tile parallel wall | 3.150925 s |
| launch-fringe estimate | 14.1 us |
| workers / logical jobs | 15 / 64 |

I tested `MinBatchSize=4` with three matched cases: wall seconds `3.590884, 3.634734,
3.753986` (avg `3.659868`) versus the final control average `3.620286`. The tile parallel bucket
was slightly lower (`3.125197` s vs `3.150925` s), but end-to-end wall clock was **1.09% slower**.
The observed worker count and logical job count stayed 15 / 64, and the launch-fringe estimate
remained about 13.1 us. This is the required bucket-win/no-wall-win case: the tuning was reverted;
the production/default setting remains `MinBatchSize=1`.

`ParallelForWithExistingTaskContext` was not adopted. The measured launch fringe is negligible
relative to 0.59 s tile work, and the mesher already uses thread-local scratch storage. No claim is
made here about the separate game workload of many small streaming submissions.

## Part 4 — renderer

No renderer rewrite or RMC change was justified. The source audit and three final render cases
showed:

- the canonical mesh is built once per explorer world;
- the triangle cache/BVH is built once and reused by all eight viewpoints;
- each viewpoint owns one `FColor` output allocation;
- the per-pixel traversal uses a fixed stack array, not a heap allocation;
- camera movement does not call the mesher again.

The three 512x288 render cases had total case wall seconds `8.704730, 8.985744, 8.517606`
(avg `8.736027`), including camera-seed walking. Raster-only diagnostics averaged 1.482921 s
for eight views, 0.286297 s for the first view, 0.170946 s per additional view, and 0.083190 s
for one-time acceleration construction. This is consistent with the owner's approximately
0.15 s/view observation; meshing benefits from Part 2 automatically. There was no renderer
change, so there is no before/after performance claim to report or keep.

## Memory/index follow-up

The existing profiler research measured 431,162,116 bytes (411.2 MiB) across 15 workers in the
large run. In the representative 128^3 breakdown, spatial-index bins and candidate arrays alone
accounted for 214,345,644 bytes, inside a 360,063,124-byte cache total. The index addition did not
show an end-to-end wall-clock gain in the prior measurements.

I did not remove it silently. The explicit follow-up proposal is an index-on/index-off batch
ablation before deletion: removing it should recover roughly that allocation, at the cost of
broader room/tunnel candidate scans and potentially slower density queries. That trade is not
proven by this task's export comparison.

## Determinism, invariants, and scope

All three final control hashes and all three final reuse hashes are `ED0F1F31`; every case reported
`json_deterministic=yes`. The three render cases also repeated the same geometry hash
`847BE4B9`. Owner regions are disjoint, tile results are merged in fixed Z/Y/X order, no RNG or
runtime CPU-feature dispatch was added, and the shared grid is read-only after its deterministic
fill. The only worker writes are to distinct cells of that run-owned output lattice; generator and
world state remain read-only.

The build succeeded with UnrealBuildTool after the final source change. Changes are limited to the
VoxelForge source and this report, with generated evidence under the plugin's `Saved` directory.
RealtimeMeshComponent was not modified. No VoxelM root, sibling plugin, `Content`, or `Config`
files were written; no links or historical worktrees were created.
