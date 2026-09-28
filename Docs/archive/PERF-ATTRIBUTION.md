# PERF-ATTRIBUTION

Measured 2026-09-12 at `cf0319d` (`experimental`). This was measurement-only: the source changes add
diagnostic counters and clocks; they do not change density or meshing decisions. The primary case is
the 128^3 `TunnelNetwork`, seed 1337, slot 1, operator-stack setting 1, origin `(-64,-64,-224)`
voxels, step 1, 64 canonical 32^3 tiles, and 15 workers.

`clean` means diagnostics off and `-blockearlyout=0`, with fused evaluation and shared density-grid
reuse enabled. The three clean primary runs used an exact-HEAD DLL staged before instrumentation:
603.599, 590.746, and 569.627 ms mesh time, mean **587.991 ms**. The requested 0.556 s is therefore
within the observed run spread but was not reproduced exactly on this host. `attr` means the final
instrumented DLL, `-perfattribution`, sample interval 128. Its wall-clock self-check compares the
same export against an unprofiled run in the same process.

The prescribed build was run successfully with:

```text
dotnet "E:\Program Files\Epic Games\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll" UnrealEditor Win64 Development "-Project=E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\HostProject.uproject" -WaitMutex -FromMsBuild -architecture=x64 -NoUBA "-Log=E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\PerfAttributionBuild.log"
```

The measured process loaded the staged runtime DLL at
`E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Binaries\Win64\UnrealEditor-VoxelForge.dll`, SHA-256
`C0CB812BB25EFE8C836DC1C1718C408DA5FE9D9F258B4AE12A6CA846510CA0B2`, length 7,673,856 bytes. The
staged editor DLL was `E64C9E80869EFA53FA1556947718DB8E64975ABF6B546657BCFF5D775A2B679A`, length
502,784 bytes. The game harness recorded the same runtime path and hash. The primary attribution and
final classifier exports returned `status=ok`, geometry CRC `07C14005`, and the required OBJ SHA-256
`b3e5f4c398dd0547c6c55f415872058246d4a292c2f0d636ee1cd0829de9b377`.

The table is the attribution. Only rows marked **additive** are summed. Profiler component rows are
sampled nested diagnostics; their CPU and worker-wall figures are evidence about composition, not
additional time to add to the wall ledger.

| Bucket / question | Measurement and mode | Elapsed time, volume, or count | Whole-clock accounting / interpretation |
|---|---|---:|---|
| Historical comparison | `34f06df` value supplied in the request | 130.000 ms | Historical pre-op-stack reference; the exact old binary was not rebuilt because that commit predates the current editor commandlet and host harness. |
| Current clean primary | 3 exports, exact `cf0319d` DLL, diagnostics **off**, fused=1, reuse=1, classifier early-out=0 | **587.991 ms mean**; 569.627–603.599 ms | Clean headline mesh clock. Mean is 4.523x the supplied 130 ms reference. |
| Primary attribution self-check | Final DLL, `attr`; profiled 606.128 ms versus same-argument unprofiled 602.341 ms | 1.0063x; +0.629% | Passed. Geometry equal. The profiler did not reproduce the 11.5x inflation seen in the earlier diagnostic mode. |
| Full commandlet export | Final DLL, `attr`; mesh plus output and bookkeeping | **1,004.557 ms** | Full export wall, not just mesh generation. The OBJ writer is a material part of this clock. |
| Full export: mesh generation | Additive export ledger | 606.128 ms; 60.338% of export | Includes the complete additive mesh ledger below. |
| Full export: OBJ/output interval | Additive; includes nested `MeasureExploreMesh` | 394.273 ms; 39.249% | `measure_seconds` is 82.964 ms inside this interval and is not added again. |
| Full export: geometry hash | Additive | 2.946 ms; 0.293% | CRC/hash bookkeeping. |
| Full export: manifest | Additive | 0.984 ms; 0.098% | Manifest write. |
| Full export: remainder | Additive export remainder | 0.225 ms; 0.022% | Export ledger remainder. Mesh + OBJ interval + hash + manifest + remainder = 1,004.557 ms. |
| Mesh ledger: preparation | **Additive** | 0.000101 ms; 0.0000% of mesh | No meaningful cost. |
| Mesh ledger: shared density-grid fill | **Additive**; 2,248,091 unique shared samples | **590.923 ms; 97.4913% of mesh** | This is the dominant remaining cost. It is the caller-side density-grid stage and includes the canonical fused `GetDensityAt` work. |
| Mesh ledger: pre-tile setup | **Additive** | 0.001602 ms; 0.0003% | No meaningful cost. |
| Mesh ledger: tile parallel wall | **Additive**; 64 jobs, 15 workers | 10.972 ms; 1.8102% | Per-tile cell scan, interpolation, normals, stream assembly, and task scheduling after the shared grid exists. |
| Mesh ledger: tile merge | **Additive** | 4.232 ms; 0.6982% | Cross-tile output merge. |
| Mesh ledger: accounting remainder | **Additive** | 0.000097 ms; 0.0000% | Stage sum 606.128205 ms versus mesh clock 606.128301 ms; `sum_matches_mesh=true`. Nothing is hidden in this remainder. |
| Mesh ledger total | Final DLL, `attr` | **606.128301 ms** | Complete mesh wall clock. |
| `GetDensityAt` parent | Final primary, `attr`; 2,248,091 calls, 17,558 sampled parent intervals, 15 workers | 580.184 ms worker-wall estimate | Nested inside the 590.923 ms density-grid stage. The remaining 10.738 ms is grid-loop/task/read overhead outside the parent scope; it is accounted for by the additive density-grid row. |
| Fused evaluator itself | Final primary, `attr`; 17,571 sampled component scopes; 2,248,091 fused tunnel samples | 107.183 ms sampled CPU; 12.301 ms sampled worker wall | Diagnostic sample only, not additive. Detail ran for 1,079,777 samples and was skipped for 1,168,314, about 52.0% of fused lattice samples. |
| Fused density core | Final primary, `attr`; sampled nested scope | 108.371 ms sampled CPU; 12.377 ms sampled worker wall | Diagnostic sample only. It is not a 108 ms total bucket and must not be added to the mesh ledger. |
| Room/tunnel graph traversal | Final primary counters | Rooms: 11,099,891 candidates / 1,730,920 evaluated; tunnels: 28,951,420 candidates / 3,462,565 evaluated; tunnel-core: 28,951,420 candidates / 1,881,812 evaluated | Room and tunnel candidate traversal is repeated inside the common density work. This is evidence of work volume, not a separately additive clock. |
| Room graph build and SDF | Final primary, `attr` | `RoomGraphBuild`: 77 calls, 192.146 ms sampled CPU / 19.323 ms worker wall; `RoomGraphSdf`: 12.101 ms / 0.989 ms sampled CPU/wall | The build is cold/cache construction work; SDF figures are nested samples. The room graph is not a large independent wall bucket on the primary fused route. |
| Tunnel-core world evaluation | Final primary, `attr` | 8.857 ms sampled CPU; 0.872 ms sampled worker wall | Nested diagnostic sample. The core traversal is present, but the measured evidence does not support calling it the whole 4.3x. |
| Post-stack structural/passage tail | Final primary, `attr`; `GetDensityAt` tail | `StructuralTail`: 20.853 ms CPU / 1.896 ms wall; `DensityStructuralPosts`: 21.909 ms / 1.990 ms; `PassageStructuralPosts`: 10.476 ms / 0.997 ms sampled CPU/wall | This is real current-only post-stack work, but it is a small sampled component relative to the 590.923 ms density-grid wall. All rows are nested and non-additive. Passage modifier was 4.579 ms CPU / 0.404 ms wall; landing-floor and room-floor samples were 1.691 ms / 0.177 ms and 0.665 ms / 0.060 ms. |
| Floor/profile work | Final primary counters, fused route; classifier figures from the separate classifier control | Primary support-column builds/queries/checks: 0 / 0 / 0; authored-floor samples: 0; floor-profile builds 2,636; segments 12,624; backstops 6,509 tunnel and 49 room. Classifier control: 211,289 support-column builds, 2,171,763 candidates, 1,370,314 floor queries, 2,296,243 checks, 9,710 authored-floor samples, 97,950 profiles and 442,278 profile segments. | The fused primary route does not execute the classifier’s support-column path. The classifier does, and that work is part of its separate 2.057 s route, not the clean 0.588 s headline. |
| Software tunnel cache | Final primary, `attr` | 2,248,043 hits, 48 misses, 77 cache builds | Cache lookup is not the removed verdict cache. The current tunnel cache is overwhelmingly hit; no hardware cache-miss counter was available. |
| Classifier absent from the clean headline | Clean exact-HEAD control, diagnostics **off**, classifier early-out=0 | 0 classifier calls | The stated 0.556 s case does not pay interval proofs. It is incorrect to charge classifier time to that baseline. |
| Classifier control when enabled | 3 clean exact-HEAD controls, diagnostics **off**, classifier early-out=1 | **2,056.596 ms mean** | +1,468.605 ms versus the clean primary mean; 3.498x the primary. This is a separate, expensive route, not the source of the clean 0.556 s. |
| Classifier total self-check | Final DLL, `attr`, classifier early-out=1 | 1,980.835 ms profiled versus 2,013.547 ms unprofiled; 0.9838x | Passed; geometry equal. Profile overhead was -1.62% in this run. |
| Interval proofs | Final classifier attribution; 32 sampled calls out of 4,096 | 494,976 refine/box calls; 479,872 whole-mixed, 15,104 whole-solid, 0 whole-air; 63,488 splits; max depth 4 | Proof detail is present and conservative, but its route is costly. The classifier’s scaled diagnostic work was 14,113.190 ms interval-box, 62.221 ms exact-core, and 399.770 ms exact-final microseconds; these are diagnostic work estimates, not additive wall time. |
| Classifier repeated room traversal | Final classifier attribution | 2,041,600 room-tail queries, 933,120 evaluated; 6,030,720 exact-core cache hits; 612,864 exact-core samples; 768 exact-final samples | The classifier does repeat room-graph work. Scaled diagnostic work was 11,423.590 ms room propagation, 4,665.818 ms exact primitive, 5,815.002 ms cache-window, and 1,227.366 ms room tail microseconds. |
| Shared grid traffic | Final primary, `attr` | 2,744,000 tile-grid samples; 2,248,091 unique shared samples; 495,909 duplicate evaluations before reuse, 0 after | The shared lattice removes duplicate evaluations, but filling the remaining 2.248 M samples is still 590.923 ms. |
| Mesher cell classification | Final primary, `attr`; nested sample | 2,097,152 cell calls; 1.344 ms sampled worker wall | The cell loop is not the dominant wall bucket. |
| Mesher vertex interpolation | Final primary, `attr`; nested sample | 76,808 calls; 0.012 ms sampled worker wall | Negligible in the observed sample. |
| Mesher gradient normals | Final primary, `attr`; nested sample | 76,808 calls; 1.056 ms sampled worker wall | Small nested cost; gradient normals come from the density grid. |
| Mesher stream building | Final primary, `attr`; nested sample | 76,808 calls; 3.140 ms sampled worker wall | Small relative to density-grid fill. Runtime stream handoff is not executed by the export commandlet. |
| Mesher output and memory | Final primary, `attr` | 82,273 vertices, 153,346 triangles; logical output arrays 7,434,716 bytes; allocated output 8,449,820 bytes; OBJ 17,587,937 bytes; estimated working set 14,851,564 bytes | Byte counters are logical allocator/output traffic, not physical DRAM traffic. |
| Grid read/write bytes | Final primary, `attr` | Density-grid bytes 10,976,000; shared-grid read bytes 10,976,000 | The instrumentation sees logical grid traffic. It does not see hardware cache misses, NUMA traffic, or memory-controller events. |
| Operator-block scratch/copy | Final primary, `attr` | 0 scratch bytes, 0 copy bytes, 0 builds, 0 operators | The old interpreted block machinery is not on the canonical primary route. |
| Operator-block control | Clean diagnostics-off controls with reuse=0 and fused=0 | 2,332.186 ms; 1,566 block builds; 2,486,750 block samples; 31,320 operator visits; 99,470,000 scratch bytes; 101,956,750 copy bytes | This is a deliberately separate block-path control, not the clean fused baseline. It demonstrates the traffic that is absent from the primary route. |
| Scalar and fused no-reuse controls | Clean diagnostics-off controls with reuse=0 | Scalar 2,733.080 ms; fused 2,218.132 ms | These controls show the cost of removing shared-grid reuse, but they are not attribution buckets for the primary run. |
| What `34f06df` did not do | Read-only source comparison at the historical commit | No active TunnelNetwork operator-stack evaluation, no fused evaluator, no current post-stack tunnel-floor/structural tail, no current editor export commandlet | The old source already had a shared density grid, `ClassifyTile`, marching-cubes cell classification, interpolation, grid-gradient normals, and stream construction. Those are not new current-only work. The old source also had room/tunnel morphology and passage carving, so “rooms/tunnels exist” alone is not a delta. |
| Primary operation-level attribution gap | Final primary profiler’s own ledger | Sampled phase sum 14.443 ms versus sampled `GetDensityAt` parent 580.184 ms; sampling gap 565.741 ms | This is the honest limit: component scopes were sampled to keep profiled/unprofiled totals within 0.63%, so their expanded operation-level wall shares are not identifiable from this run. The full mesh wall is nevertheless closed by the additive stage ledger, with a 0.000097 ms remainder. |
| Headless game clean check | `PerfAttribGameCleanFinal_20260912`; diagnostics **off**, tile profiling off; 343 LOD0-ready samples | request-to-ready p95 **0.375470 s**; queue wait p95 0.082292 s; generation p95 0.309538 s; collision cook p95 0.052945 s | Secondary check agrees with the earlier conclusion that queue wait is not the main generation constraint. |
| Headless game attribution check | `PerfAttribGameAttrFinal_20260912`; diagnostics **on**, tile profiling on; 834 profile lines, 822 applied | request-to-ready p95 **0.395715 s**; queue wait p95 0.083363 s; generation p95 0.315281 s; collision cook p95 0.061387 s | Attribution overhead was modest in the end-to-end game summary. Tile-profile p95 was 1.295847 s, of which classifier was 0.106626 s, mesh 1.284040 s, stream building 0.000361 s, room work 0.008842 s, and stack work 0.012019 s; these game tiles are not the same workload as the single 128^3 export. |

The surprise is that the clean primary is almost entirely the shared density-grid fill: the ordinary
mesher stages after it total only about 15.204 ms. The fused evaluator is genuinely on the canonical
path, and the room/tunnel candidate counters show substantial repeated traversal, but the sampled
component timers cannot honestly turn that 590.923 ms into exact per-operation percentages without
reintroducing the profiler inflation this round was designed to avoid. The classifier is an even
larger cost when enabled, yet it is absent from the clean baseline. The old path already performed
the grid and marching-cubes phases, so those phases should not be described as newly introduced
work; the current-only items are the active fused route and the later structural/floor machinery.

What could not be attributed: physical allocation-event counts, physical memory/cache misses, and an
exact numeric per-operation delta against the old `34f06df` binary. The old commit has no compatible
current export commandlet, and rebuilding it would not be the same measurement harness. The complete
current clocks, deterministic geometry, clean/profiled comparison, current counters, and all logical
memory-byte counters are accounted for above. Existing logs and outputs under `Saved\` were retained.
