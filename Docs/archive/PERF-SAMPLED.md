# PERF-SAMPLED

Measurement-only report for the in-process stack sampler, 2026-09-13. No generation, classifier, or field logic was optimized in this round.

## Bottom line

The sampler is useful: it samples registered generation workers without adding scopes to the density hot path, tags every capture with LOD, finds the planted cost, and exposes the game path's dominant work. It is not yet an acceptance-clean low-overhead instrument at the requested default interval. Static timing and worker CPU move beyond the two-run off baseline; moving worker CPU is within noise, but moving request/generation timing is not.

## Current command entry point

Use the guarded harness at `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Tools\VoxelForgeTest.ps1` for new measurements. It owns the staged host under `Saved\BuildHost`, serializes Unreal launches, uses an isolated `-userdir`, Zen/DDC directory, and log directory under the requested run directory, waits by PID, records crash/`UECC-*` evidence, and writes compact `result.json` plus a ≤40-line `summary.txt`; full logs remain beside them.

All paths passed to `-Out` must be absolute and under the plugin's `Saved` directory. The normal entry forms are:

```powershell
pwsh -NoLogo -NoProfile -File "E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Tools\VoxelForgeTest.ps1" -Scenario canonical -Build -Assets default -Label canonical
pwsh -NoLogo -NoProfile -File "E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Tools\VoxelForgeTest.ps1" -Scenario owner -Assets owner -Label owner
pwsh -NoLogo -NoProfile -File "E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Tools\VoxelForgeTest.ps1" -Scenario probe -Assets owner -Label probe
pwsh -NoLogo -NoProfile -File "E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Tools\VoxelForgeTest.ps1" -Scenario perf -Label perf
pwsh -NoLogo -NoProfile -File "E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Tools\VoxelForgeTest.ps1" -Scenario tests -Label tests
pwsh -NoLogo -NoProfile -File "E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Tools\VoxelForgeTest.ps1" -Scenario parity -Build -Assets owner -Label parity
```

Optional controls are `-Cvars @{...}`, `-Out "E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\TestHarness\named"`, and `-Label name`. `-Assets owner` stages `DA_Strate3` and `DA_Settings` and records both SHA-256 values. `parity` exports 64 level-0 tiles through `VoxelForgeExplore -gameconfig=1`, runs the headless game's streaming path with the same owner settings, dumps per-tile hashes, and fails on the first missing or differing tile. Do not invent a direct `UnrealEditor` command line: the harness's launch ledger is the reproducible command record.

The headline game result is that LOD0 is not dominated by candidate AABB rejection. It is dominated by the evaluator and its downstream tunnel/floor/core and structural work. Coarse LOD1+ samples are dominated by room-cache/player-fit/room-landing work. The profile supports the suspected swept-tunnel/core duplication, but does not isolate the parameter-copy cost.

## Instrument and build

- Registration is at generation-task entry/exit in VoxelWorld.cpp, not around individual samples. Disabled registration is one relaxed atomic load of the global sampler flag. Idle workers are not registered.
- The sampler thread calls FPlatformStackWalk::CaptureThreadStackBackTrace for each active registered worker at the configured interval (default 1000 us). Raw PCs go to preallocated bounded storage; workers share no sampling lock and do no symbolization.
- After the run, PCs are symbolized with the platform resolver and DbgHelp inline callbacks when available. Inline frames were resolved in both game runs. The final bounded store is a 131072-sample reservoir, so the long moving run is not represented by only its first capture window.
- LOD is written beside each raw sample and is set when the generation task registers. The game switch is -voxel.SampleStacks=intervalus; the commandlet switch is -samplestacks[=intervalus]. Both write under the plugin's Saved directory.
- The game logs show the sampler starting and stopping from the measured staged runtime. The measured runtime DLL was E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Binaries\Win64\UnrealEditor-VoxelForge.dll, SHA-256 FBC56BDD4826057A5EF6A9CB6D06694CF61C92DE077762AC05DB926779A68BA8. The staged editor DLL was E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Binaries\Win64\UnrealEditor-VoxelForgeEditor.dll, SHA-256 E55930A8D8D7C2F8EF1D8FFEE0867323044D5054752DF531D3A6DBC98FB2E9B8.
- Build command: dotnet "E:\Program Files\Epic Games\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll" UnrealEditor Win64 Development "-Project=E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\HostProject.uproject" -WaitMutex -FromMsBuild -architecture=x64 -NoUBA "-Log=E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\PerfSampledBuild_Fallback.log"

## Closure

| Run | Capture attempts | Registered-thread samples | Retained stacks | LOD0 | LOD1+ | Resolved frames | Unknown frames | Wait-like frames | Resolved leaf |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Static | 193166 | 193165 | 131072 | 16057 | 115015 | 59.1492% | 40.8508% | 0.0136% | 82.5142% |
| Moving | 251129 | 251126 | 131072 | 41467 | 89605 | 57.5231% | 42.4769% | 0.0370% | 83.9149% |

There were zero untagged samples and zero capture failures. The sampler captured generation work rather than waits: wait-like frames were 368 static and 957 moving, or 0.2808% and 0.7301% of retained samples. Unknown frames are a real symbol-coverage limitation, not a wait problem: 40.8508% and 42.4769% of all captured frame slots were unknown, while the leaf frame still resolved for 82.5142% and 83.9149% of retained samples. Unknown is kept as an explicit top-table row.

The static run had 7380 inline symbols and the moving run had 7833. The complete raw CSV and readable summary for the headline runs are:

- E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\VoxelStackSamples_game_18392.csv
- E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\VoxelStackSummary_game_18392.txt
- E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\VoxelStackSamples_game_13844.csv
- E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\VoxelStackSummary_game_13844.txt

## Game overhead

The off runs are the noise estimate. Worker generation seconds are the sum of generation time recorded by workers, not wall-clock time.

| Path | Run | Sampler | Request p50 | Request p95 | Generation p50 | Generation p95 | Worker generation seconds | Applied tiles | Visible tiles | Triangles | Obsolete |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Static | GameStaticOffA_20260913 | off | 0.108582 | 0.171829 | 0.076228 | 0.122883 | 184.994019 | 841 | 447 | 669834 | 0 |
| Static | GameStaticOffB_20260913 | off | 0.110888 | 0.179788 | 0.077364 | 0.128214 | 185.959906 | 841 | 447 | 669834 | 0 |
| Static | GameStaticOnFinal2_20260913 | 1 ms | 0.112669 | 0.188827 | 0.079557 | 0.130637 | 193.721335 | 841 | 447 | 669834 | 0 |
| Moving | GameMovingOffA_20260913 | off | 0.105508 | 0.191523 | 0.074358 | 0.126597 | 256.549700 | 2219 | 949 | 1862188 | 1 |
| Moving | GameMovingOffB_20260913 | off | 0.106318 | 0.194674 | 0.074501 | 0.127767 | 256.792375 | 2216 | 946 | 1860284 | 2 |
| Moving | GameMovingOnFinal2_20260913 | 1 ms | 0.108368 | 0.201022 | 0.078347 | 0.132525 | 256.742287 | 2199 | 930 | 1853788 | 4 |

Static off-run noise was 4.64% for request p95, 1.49% for generation p50, 4.34% for generation p95, and 0.52% for worker generation seconds. Against the mean of the two off runs, sampler-on static was +2.67% request p50, +7.41% request p95, +3.60% generation p50, +4.06% generation p95, and +4.71% worker generation seconds. This fails the requested within-noise test for static.

Moving off-run noise was 1.64% for request p95, 0.19% for generation p50, 0.92% for generation p95, and 0.095% for worker generation seconds. Against the off mean, sampler-on moving was +2.32% request p50, +4.10% request p95, +5.26% generation p50, +4.20% generation p95, and +0.03% worker generation seconds. Moving worker CPU passes the noise check; request and generation timing do not. Therefore small (<5%) game-path differences should not be treated as reliable until the sampling overhead is reduced or calibrated.

## Field and tile/triangle comparison

Static on/off is exact at the game output level measured here: 841 applied tiles, 447 visible tiles, and 669834 triangles in both off runs and the sampler-on run. The sampler has no field writes and classifier mode was the default 0 in every game run; no classifier was enabled.

The moving comparison is a finite 25-second streaming window, so applied work is schedule-sensitive even with the sampler off: the two off runs are already 2219/2216 applied tiles and 1/2 obsolete tile aborts. The on run is 2199 applied, 930 visible, 1853788 triangles, and 4 obsolete aborts. This is not evidence of a sampler-induced field mutation; it is evidence that the moving window did not finish the same tile frontier. The implementation remains deterministic for the same build and seed; any classifier used in a future run must remain outside sampler-controlled state and must be field-preserving.

## Static game profile (GameStaticOnFinal2_20260913)

Percentages are relative to the retained non-empty-stack samples in that subgroup. Inclusive rows count a function once per sample; they are intentionally not additive.

### All registered generation samples

#### Exclusive functions (leaf)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 22919 | 17.4858 | unknown | <unknown> | unknown |
| 2 | 19415 | 14.8125 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:77 |
| 3 | 10059 | 7.6744 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:59 |
| 4 | 7519 | 5.7365 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2392 |
| 5 | 6377 | 4.8653 | plugin | [Inline Frame] VoxelNoise::Detail::Lerp() | Plugin/Public/VoxelNoise.h:87 |
| 6 | 4228 | 3.2257 | plugin | `anonymous namespace'::VF_EvaluateRoomLandingDensity() | Plugin/Private/VoxelCaveMorphology.cpp:2515 |
| 7 | 4104 | 3.1311 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:67 |
| 8 | 4048 | 3.0884 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 9 | 3725 | 2.8419 | plugin | `anonymous namespace'::VF_BuildPlayerFitStencil() | Plugin/Private/VoxelCaveMorphology.cpp:2086 |
| 10 | 3565 | 2.7199 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2648 |
| 11 | 3522 | 2.6871 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:169 |
| 12 | 2475 | 1.8883 | plugin | [Inline Frame] ?A0x7db52731::VF_IsPointInsidePlayerCapsule() | Plugin/Private/VoxelCaveMorphology.cpp:1993 |
| 13 | 1827 | 1.3939 | plugin | [Inline Frame] VoxelHash::Mix() | Plugin/Public/VoxelCaveMorphology.h:215 |
| 14 | 1771 | 1.3512 | plugin | `anonymous namespace'::VF_ValidatePlayerFitPose() | Plugin/Private/VoxelCaveMorphology.cpp:2216 |
| 15 | 1646 | 1.2558 | engine | [Inline Frame] TArray<UE::Math::TIntPoint<int>,TSizedDefaultAllocator<32> >::Emplace() | Engine/Runtime/Core/Public/Containers/Array.h:2602 |
| 16 | 1271 | 0.9697 | plugin | `VoxelCaveMorphology::BuildChunkCache'::`2'::<lambda_7>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:5045 |
| 17 | 1263 | 0.9636 | plugin | `anonymous namespace'::VF_ProjectNativePassageFloor() | Plugin/Private/VoxelStrateManager.cpp:471 |
| 18 | 1220 | 0.9308 | plugin | UVoxelGenerator::GetDensityAt() | Plugin/Private/VoxelGenerator.cpp:1656 |
| 19 | 1024 | 0.7812 | engine | [Inline Frame] UE::Math::TVector2<double>::{ctor}() | Engine/Runtime/Core/Public/Math/Vector2D.h:837 |
| 20 | 952 | 0.7263 | engine | [Inline Frame] UE::Math::TVector<double>::operator/() | Engine/Runtime/Core/Public/Math/Vector.h:1585 |
| 21 | 897 | 0.6844 | plugin | [Inline Frame] VoxelNoise::Perlin3D() | Plugin/Public/VoxelNoise.h:111 |
| 22 | 817 | 0.6233 | engine | [Inline Frame] FGenericPlatformMath::Min() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:990 |
| 23 | 758 | 0.5783 | plugin | VoxelCaveMorphology::BuildChunkCache() | Plugin/Private/VoxelCaveMorphology.cpp:5165 |
| 24 | 742 | 0.5661 | plugin | [Inline Frame] ?A0x7db52731::VF_GetFloorReliefBound() | Plugin/Private/VoxelCaveMorphology.cpp:296 |
| 25 | 725 | 0.5531 | engine | [Inline Frame] UE4::SSE4::FloorToFloat() | Engine/Runtime/Core/Public/Math/UnrealPlatformMathSSE4.h:34 |
| 26 | 684 | 0.5219 | plugin | `VoxelCaveMorphology::EvaluateTunnelCoreWorld'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6298 |
| 27 | 640 | 0.4883 | engine | [Inline Frame] FGenericPlatformMath::Max() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:968 |
| 28 | 633 | 0.4829 | plugin | [Inline Frame] `anonymous-namespace'::VF_FindPlayerFitPointForRoom::__l2::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:2629 |
| 29 | 602 | 0.4593 | engine | [Inline Frame] TArray<FBuildRoom,TSizedInlineAllocator<64,32,TSizedDefaultAllocator<32> > >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 30 | 589 | 0.4494 | engine | [Inline Frame] UE::Math::TVector<double>::DistSquared() | Engine/Runtime/Core/Public/Math/Vector.h:2480 |

#### Inclusive functions (once per sample)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 131072 | 100.0000 | engine | [Inline Frame] Invoke() | Engine/Runtime/Core/Public/Templates/Invoke.h:47 |
| 2 | 131072 | 100.0000 | engine | [Inline Frame] LowLevelTasks::FTask::Init::__l13::<lambda_1>::operator()() | Engine/Runtime/Core/Public/Async/Fundamental/Task.h:499 |
| 3 | 131072 | 100.0000 | engine | [Inline Frame] LowLevelTasks::TTaskDelegate<LowLevelTasks::FTask * __cdecl(bool),48>::TTaskDelegateImpl<`LowLevelTasks::FTask::Init<`UE::Tasks::Private::FTaskBase::Init'::`2'::<lambda_1> >'::`13'::<lambda_1>,0>::Call() | Engine/Runtime/Core/Public/Async/Fundamental/TaskDelegate.h:162 |
| 4 | 131072 | 100.0000 | engine | [Inline Frame] UE::Tasks::Private::FTaskBase::Init::__l2::<lambda_1>::operator()() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:180 |
| 5 | 131072 | 100.0000 | engine | LowLevelTasks::TTaskDelegate<LowLevelTasks::FTask * __cdecl(bool),48>::TTaskDelegateImpl<`LowLevelTasks::FTask::Init<`UE::Tasks::Private::FTaskBase::Init'::`2'::<lambda_1> >'::`13'::<lambda_1>,0>::CallAndMove() | Engine/Runtime/Core/Public/Async/Fundamental/TaskDelegate.h:171 |
| 6 | 131072 | 100.0000 | engine | UE::Tasks::Private::FTaskBase::TryExecuteTask() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:518 |
| 7 | 131072 | 100.0000 | engine | UE::Tasks::Private::TExecutableTaskBase<`AVoxelWorld::LoadTile'::`2'::<lambda_1>,void,void>::ExecuteTask() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:898 |
| 8 | 131072 | 100.0000 | plugin | `AVoxelWorld::LoadTile'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelWorld.cpp:3766 |
| 9 | 131072 | 100.0000 | plugin | AVoxelWorld::GenerateTileResult() | Plugin/Private/VoxelWorld.cpp:4078 |
| 10 | 131029 | 99.9672 | plugin | UVoxelMarchingCubesMesher::GenerateMesh() | Plugin/Private/VoxelMarchingCubesMesher.cpp:64 |
| 11 | 130502 | 99.5651 | plugin | UVoxelGenerator::GetDensityAt() | Plugin/Private/VoxelGenerator.cpp:1587 |
| 12 | 118433 | 90.3572 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2800 |
| 13 | 104250 | 79.5364 | plugin | VoxelCaveMorphology::BuildChunkCache() | Plugin/Private/VoxelCaveMorphology.cpp:5276 |
| 14 | 96145 | 73.3528 | plugin | [Inline Frame] VoxelCaveMorphology::BuildChunkCache::__l2::<lambda_9>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:5153 |
| 15 | 96139 | 73.3482 | plugin | `anonymous namespace'::VF_FindPlayerFitPointForRoom() | Plugin/Private/VoxelCaveMorphology.cpp:2687 |
| 16 | 95273 | 72.6875 | plugin | `anonymous namespace'::VF_ValidatePlayerFitPose() | Plugin/Private/VoxelCaveMorphology.cpp:2217 |
| 17 | 68853 | 52.5307 | engine | [Inline Frame] UE::Core::Private::Function::TFunctionRefBase<UE::Core::Private::Function::FFunctionRefStoragePolicy,float __cdecl(float,float,float)>::operator()() | Engine/Runtime/Core/Public/Templates/Function.h:414 |
| 18 | 68717 | 52.4269 | engine | UE::Core::Private::Function::TFunctionRefCaller<``anonymous namespace'::VF_FindPlayerFitPointForRoom'::`2'::<lambda_1> const ,float,float,float,float>::Call() | Engine/Runtime/Core/Public/Templates/Function.h:298 |
| 19 | 67989 | 51.8715 | plugin | `anonymous namespace'::VF_EvaluateRoomLandingDensity() | Plugin/Private/VoxelCaveMorphology.cpp:2512 |
| 20 | 50851 | 38.7962 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2387 |
| 21 | 41573 | 31.7177 | plugin | [Inline Frame] VoxelNoise::Perlin3D() | Plugin/Public/VoxelNoise.h:115 |
| 22 | 20551 | 15.6792 | plugin | `anonymous namespace'::VF_BuildPlayerFitStencil() | Plugin/Private/VoxelCaveMorphology.cpp:2039 |
| 23 | 19415 | 14.8125 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:77 |
| 24 | 11934 | 9.1049 | engine | UE::Core::Private::ReallocGrow1_DoAlloc_Tiny<3,TSizedHeapAllocator<32,FMemory>::ForAnyElementType>() | Engine/Runtime/Core/Public/Containers/Array.h:463 |
| 25 | 10059 | 7.6744 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:59 |
| 26 | 6377 | 4.8653 | plugin | [Inline Frame] VoxelNoise::Detail::Lerp() | Plugin/Public/VoxelNoise.h:87 |
| 27 | 4104 | 3.1311 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:67 |
| 28 | 4048 | 3.0884 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 29 | 3753 | 2.8633 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:169 |
| 30 | 3710 | 2.8305 | plugin | UVoxelStrateManager::ApplyPassageStructuralPostsMC() | Plugin/Private/VoxelStrateManager.cpp:2585 |

#### Exclusive source lines (leaf source)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 22950 | 17.5095 | unknown | FSurfaceColumn::FSurfaceColumn() | unknown |
| 2 | 6377 | 4.8653 | plugin | [Inline Frame] VoxelNoise::Detail::Lerp() | Plugin/Public/VoxelNoise.h:87 |
| 3 | 4398 | 3.3554 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:77 |
| 4 | 4248 | 3.2410 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:83 |
| 5 | 3973 | 3.0312 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 6 | 3597 | 2.7443 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:82 |
| 7 | 3464 | 2.6428 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:59 |
| 8 | 3314 | 2.5284 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:80 |
| 9 | 3092 | 2.3590 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:170 |
| 10 | 2643 | 2.0164 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2382 |
| 11 | 2594 | 1.9791 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:58 |
| 12 | 2474 | 1.8875 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:68 |
| 13 | 2344 | 1.7883 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:79 |
| 14 | 2300 | 1.7548 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:54 |
| 15 | 2036 | 1.5533 | engine | [Inline Frame] TArray<float,TSizedDefaultAllocator<32> >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 16 | 1852 | 1.4130 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2377 |
| 17 | 1753 | 1.3374 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2392 |
| 18 | 1702 | 1.2985 | plugin | `anonymous namespace'::VF_BuildPlayerFitStencil() | Plugin/Private/VoxelCaveMorphology.cpp:2049 |
| 19 | 1701 | 1.2978 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:57 |
| 20 | 1536 | 1.1719 | engine | [Inline Frame] TArray<UE::Math::TIntPoint<int>,TSizedDefaultAllocator<32> >::Emplace() | Engine/Runtime/Core/Public/Containers/Array.h:2602 |
| 21 | 1489 | 1.1360 | plugin | [Inline Frame] ?A0x7db52731::VF_IsPointInsidePlayerCapsule() | Plugin/Private/VoxelCaveMorphology.cpp:1993 |
| 22 | 1486 | 1.1337 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:67 |
| 23 | 1485 | 1.1330 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:84 |
| 24 | 1087 | 0.8293 | plugin | `anonymous namespace'::VF_EvaluateRoomLandingDensity() | Plugin/Private/VoxelCaveMorphology.cpp:2509 |
| 25 | 1024 | 0.7812 | engine | [Inline Frame] UE::Math::TVector2<double>::{ctor}() | Engine/Runtime/Core/Public/Math/Vector2D.h:837 |
| 26 | 982 | 0.7492 | plugin | [Inline Frame] ?A0x7db52731::VF_IsPointInsidePlayerCapsule() | Plugin/Private/VoxelCaveMorphology.cpp:1995 |
| 27 | 952 | 0.7263 | engine | [Inline Frame] UE::Math::TVector<double>::operator/() | Engine/Runtime/Core/Public/Math/Vector.h:1585 |
| 28 | 817 | 0.6233 | engine | [Inline Frame] FGenericPlatformMath::Min() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:990 |
| 29 | 725 | 0.5531 | engine | [Inline Frame] UE4::SSE4::FloorToFloat() | Engine/Runtime/Core/Public/Math/UnrealPlatformMathSSE4.h:34 |
| 30 | 706 | 0.5386 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2644 |

### LOD0 (floor)

#### Exclusive functions (leaf)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 2079 | 12.9476 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2648 |
| 2 | 1870 | 11.6460 | unknown | <unknown> | unknown |
| 3 | 791 | 4.9262 | plugin | `anonymous namespace'::VF_ProjectNativePassageFloor() | Plugin/Private/VoxelStrateManager.cpp:471 |
| 4 | 705 | 4.3906 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:552 |
| 5 | 696 | 4.3346 | plugin | UVoxelGenerator::GetDensityAt() | Plugin/Private/VoxelGenerator.cpp:1656 |
| 6 | 561 | 3.4938 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:169 |
| 7 | 547 | 3.4066 | engine | [Inline Frame] UE::Math::TVector2<double>::{ctor}() | Engine/Runtime/Core/Public/Math/Vector2D.h:837 |
| 8 | 417 | 2.5970 | plugin | `VoxelCaveMorphology::EvaluateTunnelCoreWorld'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6298 |
| 9 | 406 | 2.5285 | engine | [Inline Frame] FGenericPlatformMath::Min() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:990 |
| 10 | 291 | 1.8123 | engine | [Inline Frame] TArray<float,TSizedDefaultAllocator<32> >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 11 | 246 | 1.5320 | plugin | [Inline Frame] ?A0x7db52731::VF_DistanceSquaredToAabb() | Plugin/Private/VoxelCaveMorphology.cpp:68 |
| 12 | 244 | 1.5196 | plugin | VF_EvaluatePassageLandingSDF() | Plugin/Private/VoxelCaveMorphology.cpp:4394 |
| 13 | 239 | 1.4884 | engine | [Inline Frame] UE::Math::TVector2<double>::SizeSquared() | Engine/Runtime/Core/Public/Math/Vector2D.h:1119 |
| 14 | 236 | 1.4698 | plugin | `VoxelCaveMorphology::EvaluateSDFCached'::`2'::<lambda_3>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6192 |
| 15 | 229 | 1.4262 | plugin | [Inline Frame] ?A0x7db52731::VF_ProjectTunnelSegmentXY() | Plugin/Private/VoxelCaveMorphology.cpp:950 |
| 16 | 209 | 1.3016 | plugin | `anonymous namespace'::VF_EvaluateSweptTunnelChain() | Plugin/Private/VoxelCaveMorphology.cpp:982 |
| 17 | 205 | 1.2767 | engine | [Inline Frame] TArray<UE::Math::TVector<double>,TSizedDefaultAllocator<32> >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 18 | 194 | 1.2082 | plugin | [Inline Frame] VoxelDensityProfile::FScopedTimer::{ctor}() | Plugin/Public/VoxelDensityProfile.h:393 |
| 19 | 188 | 1.1708 | plugin | ``anonymous namespace'::VF_EvaluateSweptTunnelChain'::`7'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:1058 |
| 20 | 181 | 1.1272 | plugin | UVoxelStrateManager::EvaluateModifierSDF() | Plugin/Private/VoxelStrateManager.cpp:2277 |
| 21 | 159 | 0.9902 | plugin | UVoxelMarchingCubesMesher::GenerateMesh() | Plugin/Private/VoxelMarchingCubesMesher.cpp:574 |
| 22 | 154 | 0.9591 | plugin | UVoxelStrateManager::ApplyPassageStructuralPostsMC() | Plugin/Private/VoxelStrateManager.cpp:2574 |
| 23 | 151 | 0.9404 | plugin | [Inline Frame] VoxelDensityProfile::FScopedTimer::End() | Plugin/Private/VoxelDensityProfile.cpp:975 |
| 24 | 142 | 0.8843 | plugin | [Inline Frame] VoxelPassageGeometry::ProjectWalkableTunnelFloor() | Plugin/Public/VoxelPassageGeometry.h:327 |
| 25 | 138 | 0.8594 | plugin | VoxelDensityProfile::FScopedTimer::~FScopedTimer() | Plugin/Private/VoxelDensityProfile.cpp:969 |
| 26 | 135 | 0.8408 | engine | [Inline Frame] UE4::SSE::FloorToInt32() | Engine/Runtime/Core/Public/Math/UnrealPlatformMathSSE.h:90 |
| 27 | 132 | 0.8221 | engine | [Inline Frame] FGenericPlatformMath::Max() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:968 |
| 28 | 126 | 0.7847 | plugin | UVoxelStrateManager::UsesOperatorStackForChunk() | Plugin/Private/VoxelStrateManager.cpp:4932 |
| 29 | 123 | 0.7660 | plugin | UVoxelStrateManager::ApplyPassageLandingFloorMC() | Plugin/Private/VoxelStrateManager.cpp:2472 |
| 30 | 122 | 0.7598 | plugin | ApplyDisturbances() | Plugin/Private/VoxelGenerator.cpp:533 |

#### Inclusive functions (once per sample)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 16057 | 100.0000 | engine | [Inline Frame] Invoke() | Engine/Runtime/Core/Public/Templates/Invoke.h:47 |
| 2 | 16057 | 100.0000 | engine | [Inline Frame] LowLevelTasks::FTask::Init::__l13::<lambda_1>::operator()() | Engine/Runtime/Core/Public/Async/Fundamental/Task.h:499 |
| 3 | 16057 | 100.0000 | engine | [Inline Frame] LowLevelTasks::TTaskDelegate<LowLevelTasks::FTask * __cdecl(bool),48>::TTaskDelegateImpl<`LowLevelTasks::FTask::Init<`UE::Tasks::Private::FTaskBase::Init'::`2'::<lambda_1> >'::`13'::<lambda_1>,0>::Call() | Engine/Runtime/Core/Public/Async/Fundamental/TaskDelegate.h:162 |
| 4 | 16057 | 100.0000 | engine | [Inline Frame] UE::Tasks::Private::FTaskBase::Init::__l2::<lambda_1>::operator()() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:180 |
| 5 | 16057 | 100.0000 | engine | LowLevelTasks::TTaskDelegate<LowLevelTasks::FTask * __cdecl(bool),48>::TTaskDelegateImpl<`LowLevelTasks::FTask::Init<`UE::Tasks::Private::FTaskBase::Init'::`2'::<lambda_1> >'::`13'::<lambda_1>,0>::CallAndMove() | Engine/Runtime/Core/Public/Async/Fundamental/TaskDelegate.h:171 |
| 6 | 16057 | 100.0000 | engine | UE::Tasks::Private::FTaskBase::TryExecuteTask() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:518 |
| 7 | 16057 | 100.0000 | engine | UE::Tasks::Private::TExecutableTaskBase<`AVoxelWorld::LoadTile'::`2'::<lambda_1>,void,void>::ExecuteTask() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:898 |
| 8 | 16057 | 100.0000 | plugin | `AVoxelWorld::LoadTile'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelWorld.cpp:3766 |
| 9 | 16057 | 100.0000 | plugin | AVoxelWorld::GenerateTileResult() | Plugin/Private/VoxelWorld.cpp:4078 |
| 10 | 16036 | 99.8692 | plugin | UVoxelMarchingCubesMesher::GenerateMesh() | Plugin/Private/VoxelMarchingCubesMesher.cpp:64 |
| 11 | 15732 | 97.9760 | plugin | UVoxelGenerator::GetDensityAt() | Plugin/Private/VoxelGenerator.cpp:1587 |
| 12 | 8893 | 55.3839 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2648 |
| 13 | 2323 | 14.4672 | plugin | UVoxelStrateManager::ApplyPassageStructuralPostsMC() | Plugin/Private/VoxelStrateManager.cpp:2585 |
| 14 | 2259 | 14.0686 | plugin | UVoxelStrateManager::ApplyPassageCarvingOnly() | Plugin/Private/VoxelStrateManager.cpp:2373 |
| 15 | 2226 | 13.8631 | plugin | [Inline Frame] VF_ApplyPassageCarving() | Plugin/Public/VoxelDensityPrimitives.h:75 |
| 16 | 2225 | 13.8569 | plugin | UVoxelStrateManager::EvaluateModifierSDF() | Plugin/Private/VoxelStrateManager.cpp:2259 |
| 17 | 1990 | 12.3933 | plugin | VoxelCaveMorphology::EvaluateTunnelCoreWorld() | Plugin/Private/VoxelCaveMorphology.cpp:6397 |
| 18 | 1983 | 12.3498 | plugin | VoxelCaveMorphology::EvaluateSDFCached() | Plugin/Private/VoxelCaveMorphology.cpp:6222 |
| 19 | 1899 | 11.8266 | plugin | `VoxelCaveMorphology::EvaluateTunnelCoreWorld'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6345 |
| 20 | 1856 | 11.5588 | plugin | `anonymous namespace'::VF_EvaluateSweptTunnel() | Plugin/Private/VoxelCaveMorphology.cpp:1149 |
| 21 | 1832 | 11.4094 | plugin | `anonymous namespace'::VF_ProjectNativePassageFloor() | Plugin/Private/VoxelStrateManager.cpp:470 |
| 22 | 1765 | 10.9921 | plugin | `anonymous namespace'::VF_EvaluateSweptTunnelChain() | Plugin/Private/VoxelCaveMorphology.cpp:1067 |
| 23 | 1594 | 9.9271 | plugin | `VoxelCaveMorphology::EvaluateSDFCached'::`2'::<lambda_3>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6214 |
| 24 | 1210 | 7.5357 | plugin | [Inline Frame] VF_IsWalkableTunnelAir() | Plugin/Private/VoxelStrateManager.cpp:714 |
| 25 | 1197 | 7.4547 | plugin | `anonymous namespace'::VF_ProjectPassageFloor() | Plugin/Private/VoxelStrateManager.cpp:550 |
| 26 | 886 | 5.5178 | plugin | [Inline Frame] VoxelSDF::TaperedCapsule() | Plugin/Public/VoxelCaveMorphology.h:149 |
| 27 | 770 | 4.7954 | engine | [Inline Frame] FGenericPlatformMath::Min() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:990 |
| 28 | 757 | 4.7145 | plugin | UVoxelStrateManager::ApplyPassageNativeFloorMC() | Plugin/Private/VoxelStrateManager.cpp:2658 |
| 29 | 732 | 4.5588 | plugin | VF_EvaluatePassageLandingSDF() | Plugin/Private/VoxelCaveMorphology.cpp:4394 |
| 30 | 705 | 4.3906 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:552 |

#### Exclusive source lines (leaf source)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 1871 | 11.6522 | unknown | FSurfaceColumn::FSurfaceColumn() | unknown |
| 2 | 727 | 4.5276 | engine | [Inline Frame] TArray<float,TSizedDefaultAllocator<32> >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 3 | 666 | 4.1477 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 4 | 547 | 3.4066 | engine | [Inline Frame] UE::Math::TVector2<double>::{ctor}() | Engine/Runtime/Core/Public/Math/Vector2D.h:837 |
| 5 | 482 | 3.0018 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:170 |
| 6 | 417 | 2.5970 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2648 |
| 7 | 406 | 2.5285 | engine | [Inline Frame] FGenericPlatformMath::Min() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:990 |
| 8 | 399 | 2.4849 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2644 |
| 9 | 354 | 2.2046 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2652 |
| 10 | 239 | 1.4884 | engine | [Inline Frame] UE::Math::TVector2<double>::SizeSquared() | Engine/Runtime/Core/Public/Math/Vector2D.h:1119 |
| 11 | 215 | 1.3390 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:3652 |
| 12 | 186 | 1.1584 | plugin | `VoxelCaveMorphology::EvaluateTunnelCoreWorld'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6313 |
| 13 | 177 | 1.1023 | plugin | [Inline Frame] VoxelDensityProfile::FScopedTimer::{ctor}() | Plugin/Public/VoxelDensityProfile.h:393 |
| 14 | 173 | 1.0774 | plugin | [Inline Frame] VoxelDensityProfile::FScopedTimer::End() | Plugin/Private/VoxelDensityProfile.cpp:975 |
| 15 | 163 | 1.0151 | plugin | `VoxelCaveMorphology::EvaluateSDFCached'::`2'::<lambda_3>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6192 |
| 16 | 158 | 0.9840 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:3615 |
| 17 | 157 | 0.9778 | plugin | `anonymous namespace'::VF_ProjectNativePassageFloor() | Plugin/Private/VoxelStrateManager.cpp:471 |
| 18 | 135 | 0.8408 | engine | [Inline Frame] UE4::SSE::FloorToInt32() | Engine/Runtime/Core/Public/Math/UnrealPlatformMathSSE.h:90 |
| 19 | 119 | 0.7411 | engine | [Inline Frame] UE::Math::TVector<double>::operator*() | Engine/Runtime/Core/Public/Math/Vector.h:316 |
| 20 | 109 | 0.6788 | engine | [Inline Frame] FGenericPlatformMath::Max() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:968 |
| 21 | 104 | 0.6477 | plugin | VF_EvaluatePassageLandingSDF() | Plugin/Private/VoxelCaveMorphology.cpp:4394 |
| 22 | 104 | 0.6477 | plugin | [Inline Frame] ?A0x7db52731::VF_DistanceSquaredToAabb() | Plugin/Private/VoxelCaveMorphology.cpp:62 |
| 23 | 98 | 0.6103 | engine | [Inline Frame] UE::Math::TVector<double>::DistSquared() | Engine/Runtime/Core/Public/Math/Vector.h:2480 |
| 24 | 90 | 0.5605 | engine | [Inline Frame] UE::Math::TVector<double>::operator-() | Engine/Runtime/Core/Public/Math/Vector.h:1573 |
| 25 | 90 | 0.5605 | plugin | ``anonymous namespace'::VF_EvaluateSweptTunnelChain'::`7'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:1058 |
| 26 | 89 | 0.5543 | engine | [Inline Frame] FMath::Square() | Engine/Runtime/Core/Public/Math/UnrealMathUtility.h:580 |
| 27 | 87 | 0.5418 | plugin | VoxelDensityProfile::FScopedTimer::~FScopedTimer() | Plugin/Private/VoxelDensityProfile.cpp:969 |
| 28 | 87 | 0.5418 | plugin | `anonymous namespace'::VF_ProjectNativePassageFloor() | Plugin/Private/VoxelStrateManager.cpp:452 |
| 29 | 86 | 0.5356 | plugin | [Inline Frame] ?A0x7db52731::VF_ProjectTunnelSegmentXY() | Plugin/Private/VoxelCaveMorphology.cpp:955 |
| 30 | 81 | 0.5045 | plugin | VF_EvaluatePassageLandingSDF() | Plugin/Private/VoxelCaveMorphology.cpp:4411 |

### LOD1+ (coarse/background)

#### Exclusive functions (leaf)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 21049 | 18.3011 | unknown | <unknown> | unknown |
| 2 | 19342 | 16.8169 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:77 |
| 3 | 10021 | 8.7128 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:59 |
| 4 | 7486 | 6.5087 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2392 |
| 5 | 6355 | 5.5254 | plugin | [Inline Frame] VoxelNoise::Detail::Lerp() | Plugin/Public/VoxelNoise.h:87 |
| 6 | 4217 | 3.6665 | plugin | `anonymous namespace'::VF_EvaluateRoomLandingDensity() | Plugin/Private/VoxelCaveMorphology.cpp:2587 |
| 7 | 4090 | 3.5561 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:67 |
| 8 | 3704 | 3.2204 | plugin | `anonymous namespace'::VF_BuildPlayerFitStencil() | Plugin/Private/VoxelCaveMorphology.cpp:2086 |
| 9 | 3343 | 2.9066 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 10 | 2961 | 2.5744 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:170 |
| 11 | 2469 | 2.1467 | plugin | [Inline Frame] ?A0x7db52731::VF_IsPointInsidePlayerCapsule() | Plugin/Private/VoxelCaveMorphology.cpp:1993 |
| 12 | 1802 | 1.5668 | plugin | [Inline Frame] VoxelHash::Mix() | Plugin/Public/VoxelCaveMorphology.h:217 |
| 13 | 1755 | 1.5259 | plugin | `anonymous namespace'::VF_ValidatePlayerFitPose() | Plugin/Private/VoxelCaveMorphology.cpp:2221 |
| 14 | 1639 | 1.4250 | engine | [Inline Frame] TArray<UE::Math::TIntPoint<int>,TSizedDefaultAllocator<32> >::Emplace() | Engine/Runtime/Core/Public/Containers/Array.h:2602 |
| 15 | 1486 | 1.2920 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2652 |
| 16 | 1263 | 1.0981 | plugin | `VoxelCaveMorphology::BuildChunkCache'::`2'::<lambda_7>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:5045 |
| 17 | 948 | 0.8242 | engine | [Inline Frame] UE::Math::TVector<double>::operator/() | Engine/Runtime/Core/Public/Math/Vector.h:1585 |
| 18 | 893 | 0.7764 | plugin | [Inline Frame] VoxelNoise::Perlin3D() | Plugin/Public/VoxelNoise.h:111 |
| 19 | 757 | 0.6582 | plugin | VoxelCaveMorphology::BuildChunkCache() | Plugin/Private/VoxelCaveMorphology.cpp:5165 |
| 20 | 716 | 0.6225 | engine | [Inline Frame] UE4::SSE4::FloorToFloat() | Engine/Runtime/Core/Public/Math/UnrealPlatformMathSSE4.h:34 |
| 21 | 678 | 0.5895 | plugin | [Inline Frame] ?A0x7db52731::VF_GetFloorReliefBound() | Plugin/Private/VoxelCaveMorphology.cpp:296 |
| 22 | 631 | 0.5486 | plugin | [Inline Frame] `anonymous-namespace'::VF_FindPlayerFitPointForRoom::__l2::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:2629 |
| 23 | 599 | 0.5208 | engine | [Inline Frame] TArray<FBuildRoom,TSizedInlineAllocator<64,32,TSizedDefaultAllocator<32> > >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 24 | 524 | 0.4556 | plugin | UVoxelGenerator::GetDensityAt() | Plugin/Private/VoxelGenerator.cpp:2078 |
| 25 | 508 | 0.4417 | engine | [Inline Frame] FGenericPlatformMath::Max() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:968 |
| 26 | 499 | 0.4339 | plugin | [Inline Frame] VoxelHash::SeedOffset() | Plugin/Public/VoxelCaveMorphology.h:257 |
| 27 | 491 | 0.4269 | engine | [Inline Frame] UE::Math::TVector<double>::DistSquared() | Engine/Runtime/Core/Public/Math/Vector.h:2480 |
| 28 | 477 | 0.4147 | engine | [Inline Frame] UE::Math::TVector2<double>::{ctor}() | Engine/Runtime/Core/Public/Math/Vector2D.h:837 |
| 29 | 472 | 0.4104 | plugin | `anonymous namespace'::VF_ProjectNativePassageFloor() | Plugin/Private/VoxelStrateManager.cpp:461 |
| 30 | 463 | 0.4026 | engine | [Inline Frame] FMicrosoftPlatformMathBase::IsFinite() | Engine/Runtime/Core/Public/Microsoft/MicrosoftPlatformMath.h:17 |

#### Inclusive functions (once per sample)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 115015 | 100.0000 | engine | [Inline Frame] Invoke() | Engine/Runtime/Core/Public/Templates/Invoke.h:47 |
| 2 | 115015 | 100.0000 | engine | [Inline Frame] LowLevelTasks::FTask::Init::__l13::<lambda_1>::operator()() | Engine/Runtime/Core/Public/Async/Fundamental/Task.h:499 |
| 3 | 115015 | 100.0000 | engine | [Inline Frame] LowLevelTasks::TTaskDelegate<LowLevelTasks::FTask * __cdecl(bool),48>::TTaskDelegateImpl<`LowLevelTasks::FTask::Init<`UE::Tasks::Private::FTaskBase::Init'::`2'::<lambda_1> >'::`13'::<lambda_1>,0>::Call() | Engine/Runtime/Core/Public/Async/Fundamental/TaskDelegate.h:162 |
| 4 | 115015 | 100.0000 | engine | [Inline Frame] UE::Tasks::Private::FTaskBase::Init::__l2::<lambda_1>::operator()() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:180 |
| 5 | 115015 | 100.0000 | engine | LowLevelTasks::TTaskDelegate<LowLevelTasks::FTask * __cdecl(bool),48>::TTaskDelegateImpl<`LowLevelTasks::FTask::Init<`UE::Tasks::Private::FTaskBase::Init'::`2'::<lambda_1> >'::`13'::<lambda_1>,0>::CallAndMove() | Engine/Runtime/Core/Public/Async/Fundamental/TaskDelegate.h:171 |
| 6 | 115015 | 100.0000 | engine | UE::Tasks::Private::FTaskBase::TryExecuteTask() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:518 |
| 7 | 115015 | 100.0000 | engine | UE::Tasks::Private::TExecutableTaskBase<`AVoxelWorld::LoadTile'::`2'::<lambda_1>,void,void>::ExecuteTask() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:898 |
| 8 | 115015 | 100.0000 | plugin | `AVoxelWorld::LoadTile'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelWorld.cpp:3766 |
| 9 | 115015 | 100.0000 | plugin | AVoxelWorld::GenerateTileResult() | Plugin/Private/VoxelWorld.cpp:4078 |
| 10 | 114993 | 99.9809 | plugin | UVoxelMarchingCubesMesher::GenerateMesh() | Plugin/Private/VoxelMarchingCubesMesher.cpp:574 |
| 11 | 114770 | 99.7870 | plugin | UVoxelGenerator::GetDensityAt() | Plugin/Private/VoxelGenerator.cpp:2073 |
| 12 | 109540 | 95.2398 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2800 |
| 13 | 103850 | 90.2926 | plugin | VoxelCaveMorphology::BuildChunkCache() | Plugin/Private/VoxelCaveMorphology.cpp:5276 |
| 14 | 95772 | 83.2691 | plugin | [Inline Frame] VoxelCaveMorphology::BuildChunkCache::__l2::<lambda_9>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:5153 |
| 15 | 95766 | 83.2639 | plugin | `anonymous namespace'::VF_FindPlayerFitPointForRoom() | Plugin/Private/VoxelCaveMorphology.cpp:2687 |
| 16 | 94908 | 82.5179 | plugin | `anonymous namespace'::VF_ValidatePlayerFitPose() | Plugin/Private/VoxelCaveMorphology.cpp:2217 |
| 17 | 68607 | 59.6505 | engine | [Inline Frame] UE::Core::Private::Function::TFunctionRefBase<UE::Core::Private::Function::FFunctionRefStoragePolicy,float __cdecl(float,float,float)>::operator()() | Engine/Runtime/Core/Public/Templates/Function.h:414 |
| 18 | 68472 | 59.5331 | engine | UE::Core::Private::Function::TFunctionRefCaller<``anonymous namespace'::VF_FindPlayerFitPointForRoom'::`2'::<lambda_1> const ,float,float,float,float>::Call() | Engine/Runtime/Core/Public/Templates/Function.h:298 |
| 19 | 67746 | 58.9019 | plugin | `anonymous namespace'::VF_EvaluateRoomLandingDensity() | Plugin/Private/VoxelCaveMorphology.cpp:2512 |
| 20 | 50654 | 44.0412 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2387 |
| 21 | 41417 | 36.0101 | plugin | [Inline Frame] VoxelNoise::Perlin3D() | Plugin/Public/VoxelNoise.h:115 |
| 22 | 20462 | 17.7907 | plugin | `anonymous namespace'::VF_BuildPlayerFitStencil() | Plugin/Private/VoxelCaveMorphology.cpp:2039 |
| 23 | 19342 | 16.8169 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:77 |
| 24 | 11885 | 10.3334 | engine | UE::Core::Private::ReallocGrow1_DoAlloc_Tiny<3,TSizedHeapAllocator<32,FMemory>::ForAnyElementType>() | Engine/Runtime/Core/Public/Containers/Array.h:463 |
| 25 | 10021 | 8.7128 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:59 |
| 26 | 6355 | 5.5254 | plugin | [Inline Frame] VoxelNoise::Detail::Lerp() | Plugin/Public/VoxelNoise.h:87 |
| 27 | 4090 | 3.5561 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:67 |
| 28 | 3343 | 2.9066 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 29 | 3207 | 2.7883 | plugin | [Inline Frame] VoxelSDF::SmoothMax() | Plugin/Public/VoxelCaveMorphology.h:179 |
| 30 | 3178 | 2.7631 | plugin | [Inline Frame] VoxelSDF::Ellipsoid() | Plugin/Public/VoxelCaveMorphology.h:96 |

#### Exclusive source lines (leaf source)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 21079 | 18.3272 | unknown | <unknown> | unknown |
| 2 | 6355 | 5.5254 | plugin | [Inline Frame] VoxelNoise::Detail::Lerp() | Plugin/Public/VoxelNoise.h:87 |
| 3 | 4381 | 3.8091 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:77 |
| 4 | 4230 | 3.6778 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:83 |
| 5 | 3584 | 3.1161 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:82 |
| 6 | 3448 | 2.9979 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:59 |
| 7 | 3307 | 2.8753 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 8 | 3305 | 2.8735 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:80 |
| 9 | 2629 | 2.2858 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2382 |
| 10 | 2610 | 2.2693 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:170 |
| 11 | 2588 | 2.2501 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:58 |
| 12 | 2465 | 2.1432 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:68 |
| 13 | 2336 | 2.0310 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:79 |
| 14 | 2296 | 1.9963 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:54 |
| 15 | 1846 | 1.6050 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2377 |
| 16 | 1747 | 1.5189 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2392 |
| 17 | 1696 | 1.4746 | plugin | `anonymous namespace'::VF_BuildPlayerFitStencil() | Plugin/Private/VoxelCaveMorphology.cpp:2049 |
| 18 | 1689 | 1.4685 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:57 |
| 19 | 1528 | 1.3285 | engine | [Inline Frame] TArray<UE::Math::TIntPoint<int>,TSizedDefaultAllocator<32> >::Emplace() | Engine/Runtime/Core/Public/Containers/Array.h:2602 |
| 20 | 1486 | 1.2920 | plugin | [Inline Frame] ?A0x7db52731::VF_IsPointInsidePlayerCapsule() | Plugin/Private/VoxelCaveMorphology.cpp:1993 |
| 21 | 1481 | 1.2877 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:67 |
| 22 | 1477 | 1.2842 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:84 |
| 23 | 1309 | 1.1381 | engine | [Inline Frame] TArray<float,TSizedDefaultAllocator<32> >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 24 | 1084 | 0.9425 | plugin | `anonymous namespace'::VF_EvaluateRoomLandingDensity() | Plugin/Private/VoxelCaveMorphology.cpp:2509 |
| 25 | 979 | 0.8512 | plugin | [Inline Frame] ?A0x7db52731::VF_IsPointInsidePlayerCapsule() | Plugin/Private/VoxelCaveMorphology.cpp:1995 |
| 26 | 948 | 0.8242 | engine | [Inline Frame] UE::Math::TVector<double>::operator/() | Engine/Runtime/Core/Public/Math/Vector.h:1585 |
| 27 | 716 | 0.6225 | engine | [Inline Frame] UE4::SSE4::FloorToFloat() | Engine/Runtime/Core/Public/Math/UnrealPlatformMathSSE4.h:34 |
| 28 | 631 | 0.5486 | plugin | [Inline Frame] `anonymous-namespace'::VF_FindPlayerFitPointForRoom::__l2::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:2629 |
| 29 | 617 | 0.5365 | plugin | [Inline Frame] VoxelHash::Mix() | Plugin/Public/VoxelCaveMorphology.h:215 |
| 30 | 610 | 0.5304 | plugin | `anonymous namespace'::VF_BuildPlayerFitStencil() | Plugin/Private/VoxelCaveMorphology.cpp:2051 |

## Moving game profile (GameMovingOnFinal2_20260913)

Percentages are relative to the retained non-empty-stack samples in that subgroup. Inclusive rows count a function once per sample; they are intentionally not additive.

### All registered generation samples

#### Exclusive functions (leaf)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 21083 | 16.0851 | unknown | <unknown> | unknown |
| 2 | 13476 | 10.2814 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:77 |
| 3 | 8432 | 6.4331 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2648 |
| 4 | 7150 | 5.4550 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:59 |
| 5 | 5327 | 4.0642 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2382 |
| 6 | 4500 | 3.4332 | plugin | [Inline Frame] VoxelNoise::Detail::Lerp() | Plugin/Public/VoxelNoise.h:87 |
| 7 | 4246 | 3.2394 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 8 | 3562 | 2.7176 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:170 |
| 9 | 2987 | 2.2789 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:67 |
| 10 | 2944 | 2.2461 | plugin | `anonymous namespace'::VF_EvaluateRoomLandingDensity() | Plugin/Private/VoxelCaveMorphology.cpp:2509 |
| 11 | 2684 | 2.0477 | plugin | UVoxelGenerator::GetDensityAt() | Plugin/Private/VoxelGenerator.cpp:1673 |
| 12 | 2644 | 2.0172 | plugin | `anonymous namespace'::VF_BuildPlayerFitStencil() | Plugin/Private/VoxelCaveMorphology.cpp:2049 |
| 13 | 2211 | 1.6869 | plugin | `anonymous namespace'::VF_ProjectNativePassageFloor() | Plugin/Private/VoxelStrateManager.cpp:452 |
| 14 | 1964 | 1.4984 | engine | [Inline Frame] UE::Math::TVector2<double>::{ctor}() | Engine/Runtime/Core/Public/Math/Vector2D.h:837 |
| 15 | 1845 | 1.4076 | plugin | `VoxelCaveMorphology::EvaluateTunnelCoreWorld'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6313 |
| 16 | 1668 | 1.2726 | plugin | [Inline Frame] ?A0x7db52731::VF_IsPointInsidePlayerCapsule() | Plugin/Private/VoxelCaveMorphology.cpp:1993 |
| 17 | 1373 | 1.0475 | engine | [Inline Frame] FGenericPlatformMath::Min() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:990 |
| 18 | 1360 | 1.0376 | plugin | `VoxelCaveMorphology::EvaluateSDFCached'::`2'::<lambda_3>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6214 |
| 19 | 1271 | 0.9697 | plugin | [Inline Frame] ?A0x7db52731::VF_DistanceSquaredToAabb() | Plugin/Private/VoxelCaveMorphology.cpp:68 |
| 20 | 1251 | 0.9544 | plugin | [Inline Frame] VoxelHash::Mix() | Plugin/Public/VoxelCaveMorphology.h:215 |
| 21 | 1241 | 0.9468 | plugin | `anonymous namespace'::VF_ValidatePlayerFitPose() | Plugin/Private/VoxelCaveMorphology.cpp:2207 |
| 22 | 1150 | 0.8774 | engine | [Inline Frame] TArray<UE::Math::TIntPoint<int>,TSizedDefaultAllocator<32> >::Emplace() | Engine/Runtime/Core/Public/Containers/Array.h:2602 |
| 23 | 994 | 0.7584 | plugin | `VoxelCaveMorphology::BuildChunkCache'::`2'::<lambda_7>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:5047 |
| 24 | 921 | 0.7027 | engine | [Inline Frame] TArray<float,TSizedDefaultAllocator<32> >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 25 | 883 | 0.6737 | plugin | `anonymous namespace'::VF_EvaluateSweptTunnelChain() | Plugin/Private/VoxelCaveMorphology.cpp:971 |
| 26 | 813 | 0.6203 | plugin | [Inline Frame] ?A0x7db52731::VF_ProjectTunnelSegmentXY() | Plugin/Private/VoxelCaveMorphology.cpp:950 |
| 27 | 757 | 0.5775 | plugin | ``anonymous namespace'::VF_EvaluateSweptTunnelChain'::`7'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:1058 |
| 28 | 745 | 0.5684 | plugin | VoxelDensityProfile::FScopedTimer::~FScopedTimer() | Plugin/Private/VoxelDensityProfile.cpp:969 |
| 29 | 731 | 0.5577 | engine | [Inline Frame] UE::Math::TVector<double>::operator/() | Engine/Runtime/Core/Public/Math/Vector.h:1585 |
| 30 | 703 | 0.5363 | engine | [Inline Frame] FGenericPlatformMath::Max() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:968 |

#### Inclusive functions (once per sample)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 131072 | 100.0000 | engine | [Inline Frame] Invoke() | Engine/Runtime/Core/Public/Templates/Invoke.h:47 |
| 2 | 131072 | 100.0000 | engine | [Inline Frame] LowLevelTasks::FTask::Init::__l13::<lambda_1>::operator()() | Engine/Runtime/Core/Public/Async/Fundamental/Task.h:499 |
| 3 | 131072 | 100.0000 | engine | [Inline Frame] LowLevelTasks::TTaskDelegate<LowLevelTasks::FTask * __cdecl(bool),48>::TTaskDelegateImpl<`LowLevelTasks::FTask::Init<`UE::Tasks::Private::FTaskBase::Init'::`2'::<lambda_1> >'::`13'::<lambda_1>,0>::Call() | Engine/Runtime/Core/Public/Async/Fundamental/TaskDelegate.h:162 |
| 4 | 131072 | 100.0000 | engine | [Inline Frame] UE::Tasks::Private::FTaskBase::Init::__l2::<lambda_1>::operator()() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:180 |
| 5 | 131072 | 100.0000 | engine | LowLevelTasks::TTaskDelegate<LowLevelTasks::FTask * __cdecl(bool),48>::TTaskDelegateImpl<`LowLevelTasks::FTask::Init<`UE::Tasks::Private::FTaskBase::Init'::`2'::<lambda_1> >'::`13'::<lambda_1>,0>::CallAndMove() | Engine/Runtime/Core/Public/Async/Fundamental/TaskDelegate.h:171 |
| 6 | 131072 | 100.0000 | engine | UE::Tasks::Private::FTaskBase::TryExecuteTask() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:518 |
| 7 | 131072 | 100.0000 | engine | UE::Tasks::Private::TExecutableTaskBase<`AVoxelWorld::LoadTile'::`2'::<lambda_1>,void,void>::ExecuteTask() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:898 |
| 8 | 131072 | 100.0000 | plugin | `AVoxelWorld::LoadTile'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelWorld.cpp:3766 |
| 9 | 131071 | 99.9992 | plugin | AVoxelWorld::GenerateTileResult() | Plugin/Private/VoxelWorld.cpp:4078 |
| 10 | 130985 | 99.9336 | plugin | UVoxelMarchingCubesMesher::GenerateMesh() | Plugin/Private/VoxelMarchingCubesMesher.cpp:574 |
| 11 | 129786 | 99.0189 | plugin | UVoxelGenerator::GetDensityAt() | Plugin/Private/VoxelGenerator.cpp:2449 |
| 12 | 105813 | 80.7289 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2800 |
| 13 | 73739 | 56.2584 | plugin | VoxelCaveMorphology::BuildChunkCache() | Plugin/Private/VoxelCaveMorphology.cpp:5276 |
| 14 | 67864 | 51.7761 | plugin | [Inline Frame] VoxelCaveMorphology::BuildChunkCache::__l2::<lambda_9>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:5153 |
| 15 | 67861 | 51.7738 | plugin | `anonymous namespace'::VF_FindPlayerFitPointForRoom() | Plugin/Private/VoxelCaveMorphology.cpp:2687 |
| 16 | 67251 | 51.3084 | plugin | `anonymous namespace'::VF_ValidatePlayerFitPose() | Plugin/Private/VoxelCaveMorphology.cpp:2217 |
| 17 | 48451 | 36.9652 | engine | [Inline Frame] UE::Core::Private::Function::TFunctionRefBase<UE::Core::Private::Function::FFunctionRefStoragePolicy,float __cdecl(float,float,float)>::operator()() | Engine/Runtime/Core/Public/Templates/Function.h:414 |
| 18 | 48320 | 36.8652 | engine | UE::Core::Private::Function::TFunctionRefCaller<``anonymous namespace'::VF_FindPlayerFitPointForRoom'::`2'::<lambda_1> const ,float,float,float,float>::Call() | Engine/Runtime/Core/Public/Templates/Function.h:298 |
| 19 | 47804 | 36.4716 | plugin | `anonymous namespace'::VF_EvaluateRoomLandingDensity() | Plugin/Private/VoxelCaveMorphology.cpp:2512 |
| 20 | 35692 | 27.2308 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2387 |
| 21 | 29215 | 22.2893 | plugin | [Inline Frame] VoxelNoise::Perlin3D() | Plugin/Public/VoxelNoise.h:100 |
| 22 | 14512 | 11.0718 | plugin | `anonymous namespace'::VF_BuildPlayerFitStencil() | Plugin/Private/VoxelCaveMorphology.cpp:2039 |
| 23 | 13476 | 10.2814 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:77 |
| 24 | 8855 | 6.7558 | plugin | VoxelCaveMorphology::EvaluateSDFCached() | Plugin/Private/VoxelCaveMorphology.cpp:6222 |
| 25 | 8494 | 6.4804 | engine | UE::Core::Private::ReallocGrow1_DoAlloc_Tiny<3,TSizedHeapAllocator<32,FMemory>::ForAnyElementType>() | Engine/Runtime/Core/Public/Containers/Array.h:463 |
| 26 | 7483 | 5.7091 | plugin | VoxelCaveMorphology::EvaluateTunnelCoreWorld() | Plugin/Private/VoxelCaveMorphology.cpp:6397 |
| 27 | 7217 | 5.5061 | plugin | `VoxelCaveMorphology::EvaluateTunnelCoreWorld'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6390 |
| 28 | 7156 | 5.4596 | plugin | `VoxelCaveMorphology::EvaluateSDFCached'::`2'::<lambda_3>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6214 |
| 29 | 7152 | 5.4565 | plugin | `anonymous namespace'::VF_EvaluateSweptTunnel() | Plugin/Private/VoxelCaveMorphology.cpp:1149 |
| 30 | 7150 | 5.4550 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:59 |

#### Exclusive source lines (leaf source)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 21101 | 16.0988 | unknown | <unknown> | unknown |
| 2 | 4500 | 3.4332 | plugin | [Inline Frame] VoxelNoise::Detail::Lerp() | Plugin/Public/VoxelNoise.h:87 |
| 3 | 4103 | 3.1303 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 4 | 3252 | 2.4811 | engine | [Inline Frame] TArray<FBuildRoom,TSizedInlineAllocator<64,32,TSizedDefaultAllocator<32> > >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 5 | 3098 | 2.3636 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:170 |
| 6 | 3004 | 2.2919 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:77 |
| 7 | 2865 | 2.1858 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:83 |
| 8 | 2531 | 1.9310 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:82 |
| 9 | 2503 | 1.9096 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:59 |
| 10 | 2422 | 1.8478 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:80 |
| 11 | 1964 | 1.4984 | engine | [Inline Frame] UE::Math::TVector2<double>::{ctor}() | Engine/Runtime/Core/Public/Math/Vector2D.h:837 |
| 12 | 1867 | 1.4244 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2382 |
| 13 | 1821 | 1.3893 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:68 |
| 14 | 1817 | 1.3863 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:58 |
| 15 | 1714 | 1.3077 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2644 |
| 16 | 1705 | 1.3008 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2648 |
| 17 | 1612 | 1.2299 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:54 |
| 18 | 1603 | 1.2230 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:79 |
| 19 | 1584 | 1.2085 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2652 |
| 20 | 1373 | 1.0475 | engine | [Inline Frame] FGenericPlatformMath::Min() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:990 |
| 21 | 1295 | 0.9880 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2377 |
| 22 | 1218 | 0.9293 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:57 |
| 23 | 1216 | 0.9277 | plugin | `anonymous namespace'::VF_BuildPlayerFitStencil() | Plugin/Private/VoxelCaveMorphology.cpp:2049 |
| 24 | 1214 | 0.9262 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2392 |
| 25 | 1063 | 0.8110 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:67 |
| 26 | 1043 | 0.7957 | engine | [Inline Frame] TArray<UE::Math::TIntPoint<int>,TSizedDefaultAllocator<32> >::Emplace() | Engine/Runtime/Core/Public/Containers/Array.h:2602 |
| 27 | 1025 | 0.7820 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:84 |
| 28 | 1018 | 0.7767 | plugin | [Inline Frame] ?A0x7db52731::VF_IsPointInsidePlayerCapsule() | Plugin/Private/VoxelCaveMorphology.cpp:1993 |
| 29 | 951 | 0.7256 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:3652 |
| 30 | 943 | 0.7195 | plugin | `VoxelCaveMorphology::EvaluateSDFCached'::`2'::<lambda_3>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6192 |

### LOD0 (floor)

#### Exclusive functions (leaf)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 5755 | 13.8785 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2648 |
| 2 | 5003 | 12.0650 | unknown | <unknown> | unknown |
| 3 | 1859 | 4.4831 | plugin | UVoxelGenerator::GetDensityAt() | Plugin/Private/VoxelGenerator.cpp:1771 |
| 4 | 1589 | 3.8320 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 5 | 1554 | 3.7476 | plugin | `anonymous namespace'::VF_ProjectNativePassageFloor() | Plugin/Private/VoxelStrateManager.cpp:466 |
| 6 | 1372 | 3.3087 | engine | [Inline Frame] UE::Math::TVector2<double>::{ctor}() | Engine/Runtime/Core/Public/Math/Vector2D.h:837 |
| 7 | 1319 | 3.1808 | plugin | `VoxelCaveMorphology::EvaluateTunnelCoreWorld'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6313 |
| 8 | 1281 | 3.0892 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:170 |
| 9 | 974 | 2.3489 | plugin | `VoxelCaveMorphology::EvaluateSDFCached'::`2'::<lambda_3>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6214 |
| 10 | 906 | 2.1849 | plugin | [Inline Frame] ?A0x7db52731::VF_DistanceSquaredToAabb() | Plugin/Private/VoxelCaveMorphology.cpp:71 |
| 11 | 891 | 2.1487 | engine | [Inline Frame] FGenericPlatformMath::Min() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:990 |
| 12 | 668 | 1.6109 | engine | [Inline Frame] TArray<float,TSizedDefaultAllocator<32> >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 13 | 654 | 1.5772 | plugin | `anonymous namespace'::VF_EvaluateSweptTunnelChain() | Plugin/Private/VoxelCaveMorphology.cpp:971 |
| 14 | 600 | 1.4469 | plugin | [Inline Frame] ?A0x7db52731::VF_ProjectTunnelSegmentXY() | Plugin/Private/VoxelCaveMorphology.cpp:950 |
| 15 | 541 | 1.3047 | plugin | ``anonymous namespace'::VF_EvaluateSweptTunnelChain'::`7'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:1056 |
| 16 | 516 | 1.2444 | plugin | VF_EvaluatePassageLandingSDF() | Plugin/Private/VoxelCaveMorphology.cpp:4415 |
| 17 | 486 | 1.1720 | engine | [Inline Frame] UE::Math::TVector2<double>::SizeSquared() | Engine/Runtime/Core/Public/Math/Vector2D.h:1119 |
| 18 | 479 | 1.1551 | plugin | VoxelDensityProfile::FScopedTimer::~FScopedTimer() | Plugin/Private/VoxelDensityProfile.cpp:969 |
| 19 | 468 | 1.1286 | engine | [Inline Frame] TArray<UE::Math::TVector<double>,TSizedDefaultAllocator<32> >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 20 | 460 | 1.1093 | plugin | [Inline Frame] VoxelDensityProfile::FScopedTimer::{ctor}() | Plugin/Public/VoxelDensityProfile.h:393 |
| 21 | 407 | 0.9815 | plugin | UVoxelStrateManager::EvaluateModifierSDF() | Plugin/Private/VoxelStrateManager.cpp:2277 |
| 22 | 406 | 0.9791 | plugin | UVoxelMarchingCubesMesher::GenerateMesh() | Plugin/Private/VoxelMarchingCubesMesher.cpp:574 |
| 23 | 397 | 0.9574 | plugin | ApplyDisturbances() | Plugin/Private/VoxelGenerator.cpp:533 |
| 24 | 385 | 0.9284 | plugin | [Inline Frame] VoxelDensityProfile::FScopedTimer::End() | Plugin/Private/VoxelDensityProfile.cpp:975 |
| 25 | 361 | 0.8706 | plugin | UVoxelStrateManager::ApplyPassageStructuralPostsMC() | Plugin/Private/VoxelStrateManager.cpp:2521 |
| 26 | 328 | 0.7910 | plugin | `anonymous namespace'::VF_GetNearbyPassages() | Plugin/Private/VoxelStrateManager.cpp:246 |
| 27 | 327 | 0.7886 | engine | [Inline Frame] UE4::SSE::FloorToInt32() | Engine/Runtime/Core/Public/Math/UnrealPlatformMathSSE.h:90 |
| 28 | 320 | 0.7717 | plugin | UVoxelStrateManager::UsesOperatorStackForChunk() | Plugin/Private/VoxelStrateManager.cpp:4932 |
| 29 | 313 | 0.7548 | engine | [Inline Frame] TArray<FCachedTunnel,TSizedDefaultAllocator<32> >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 30 | 309 | 0.7452 | plugin | `VoxelCaveMorphology::EvaluateSDFCached'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6060 |

#### Inclusive functions (once per sample)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 41467 | 100.0000 | engine | [Inline Frame] Invoke() | Engine/Runtime/Core/Public/Templates/Invoke.h:47 |
| 2 | 41467 | 100.0000 | engine | [Inline Frame] LowLevelTasks::FTask::Init::__l13::<lambda_1>::operator()() | Engine/Runtime/Core/Public/Async/Fundamental/Task.h:499 |
| 3 | 41467 | 100.0000 | engine | [Inline Frame] LowLevelTasks::TTaskDelegate<LowLevelTasks::FTask * __cdecl(bool),48>::TTaskDelegateImpl<`LowLevelTasks::FTask::Init<`UE::Tasks::Private::FTaskBase::Init'::`2'::<lambda_1> >'::`13'::<lambda_1>,0>::Call() | Engine/Runtime/Core/Public/Async/Fundamental/TaskDelegate.h:162 |
| 4 | 41467 | 100.0000 | engine | [Inline Frame] UE::Tasks::Private::FTaskBase::Init::__l2::<lambda_1>::operator()() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:180 |
| 5 | 41467 | 100.0000 | engine | LowLevelTasks::TTaskDelegate<LowLevelTasks::FTask * __cdecl(bool),48>::TTaskDelegateImpl<`LowLevelTasks::FTask::Init<`UE::Tasks::Private::FTaskBase::Init'::`2'::<lambda_1> >'::`13'::<lambda_1>,0>::CallAndMove() | Engine/Runtime/Core/Public/Async/Fundamental/TaskDelegate.h:171 |
| 6 | 41467 | 100.0000 | engine | UE::Tasks::Private::FTaskBase::TryExecuteTask() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:518 |
| 7 | 41467 | 100.0000 | engine | UE::Tasks::Private::TExecutableTaskBase<`AVoxelWorld::LoadTile'::`2'::<lambda_1>,void,void>::ExecuteTask() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:898 |
| 8 | 41467 | 100.0000 | plugin | `AVoxelWorld::LoadTile'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelWorld.cpp:3766 |
| 9 | 41466 | 99.9976 | plugin | AVoxelWorld::GenerateTileResult() | Plugin/Private/VoxelWorld.cpp:4078 |
| 10 | 41416 | 99.8770 | plugin | UVoxelMarchingCubesMesher::GenerateMesh() | Plugin/Private/VoxelMarchingCubesMesher.cpp:574 |
| 11 | 40598 | 97.9044 | plugin | UVoxelGenerator::GetDensityAt() | Plugin/Private/VoxelGenerator.cpp:2449 |
| 12 | 24328 | 58.6683 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2800 |
| 13 | 6307 | 15.2097 | plugin | VoxelCaveMorphology::EvaluateSDFCached() | Plugin/Private/VoxelCaveMorphology.cpp:6222 |
| 14 | 5363 | 12.9332 | plugin | VoxelCaveMorphology::EvaluateTunnelCoreWorld() | Plugin/Private/VoxelCaveMorphology.cpp:6397 |
| 15 | 5178 | 12.4870 | plugin | `VoxelCaveMorphology::EvaluateTunnelCoreWorld'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6390 |
| 16 | 5148 | 12.4147 | plugin | `anonymous namespace'::VF_EvaluateSweptTunnel() | Plugin/Private/VoxelCaveMorphology.cpp:1149 |
| 17 | 5145 | 12.4075 | plugin | `VoxelCaveMorphology::EvaluateSDFCached'::`2'::<lambda_3>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6214 |
| 18 | 4937 | 11.9059 | plugin | `anonymous namespace'::VF_EvaluateSweptTunnelChain() | Plugin/Private/VoxelCaveMorphology.cpp:992 |
| 19 | 4710 | 11.3584 | plugin | UVoxelStrateManager::ApplyPassageStructuralPostsMC() | Plugin/Private/VoxelStrateManager.cpp:2574 |
| 20 | 4663 | 11.2451 | plugin | UVoxelStrateManager::ApplyPassageCarvingOnly() | Plugin/Private/VoxelStrateManager.cpp:2373 |
| 21 | 4561 | 10.9991 | plugin | [Inline Frame] VF_ApplyPassageCarving() | Plugin/Public/VoxelDensityPrimitives.h:75 |
| 22 | 4556 | 10.9870 | plugin | UVoxelStrateManager::EvaluateModifierSDF() | Plugin/Private/VoxelStrateManager.cpp:2234 |
| 23 | 3711 | 8.9493 | plugin | `anonymous namespace'::VF_ProjectNativePassageFloor() | Plugin/Private/VoxelStrateManager.cpp:466 |
| 24 | 2459 | 5.9300 | plugin | [Inline Frame] VF_IsWalkableTunnelAir() | Plugin/Private/VoxelStrateManager.cpp:714 |
| 25 | 2441 | 5.8866 | plugin | `anonymous namespace'::VF_ProjectPassageFloor() | Plugin/Private/VoxelStrateManager.cpp:550 |
| 26 | 1994 | 4.8086 | plugin | [Inline Frame] VoxelSDF::TaperedCapsule() | Plugin/Public/VoxelCaveMorphology.h:159 |
| 27 | 1679 | 4.0490 | plugin | ``anonymous namespace'::VF_EvaluateSweptTunnelChain'::`7'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:1058 |
| 28 | 1663 | 4.0104 | plugin | UVoxelStrateManager::ApplyPassageNativeFloorMC() | Plugin/Private/VoxelStrateManager.cpp:2658 |
| 29 | 1643 | 3.9622 | plugin | VoxelCaveMorphology::BuildChunkCache() | Plugin/Private/VoxelCaveMorphology.cpp:5276 |
| 30 | 1615 | 3.8947 | engine | [Inline Frame] FGenericPlatformMath::Min() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:990 |

#### Exclusive source lines (leaf source)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 5004 | 12.0674 | unknown | <unknown> | unknown |
| 2 | 1888 | 4.5530 | engine | [Inline Frame] TArray<FVoxelPassage,TSizedDefaultAllocator<32> >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 3 | 1489 | 3.5908 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 4 | 1372 | 3.3087 | engine | [Inline Frame] UE::Math::TVector2<double>::{ctor}() | Engine/Runtime/Core/Public/Math/Vector2D.h:837 |
| 5 | 1150 | 2.7733 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2644 |
| 6 | 1146 | 2.7636 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2648 |
| 7 | 1107 | 2.6696 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:170 |
| 8 | 1081 | 2.6069 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2652 |
| 9 | 891 | 2.1487 | engine | [Inline Frame] FGenericPlatformMath::Min() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:990 |
| 10 | 671 | 1.6182 | plugin | `VoxelCaveMorphology::EvaluateSDFCached'::`2'::<lambda_3>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6192 |
| 11 | 646 | 1.5579 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:3652 |
| 12 | 616 | 1.4855 | plugin | `VoxelCaveMorphology::EvaluateTunnelCoreWorld'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6313 |
| 13 | 486 | 1.1720 | engine | [Inline Frame] UE::Math::TVector2<double>::SizeSquared() | Engine/Runtime/Core/Public/Math/Vector2D.h:1119 |
| 14 | 484 | 1.1672 | plugin | VoxelDensityProfile::FScopedTimer::End() | Plugin/Private/VoxelDensityProfile.cpp:975 |
| 15 | 447 | 1.0780 | plugin | [Inline Frame] VoxelDensityProfile::FScopedTimer::{ctor}() | Plugin/Public/VoxelDensityProfile.h:393 |
| 16 | 344 | 0.8296 | plugin | [Inline Frame] ?A0x7db52731::VF_DistanceSquaredToAabb() | Plugin/Private/VoxelCaveMorphology.cpp:62 |
| 17 | 343 | 0.8272 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:3615 |
| 18 | 327 | 0.7886 | engine | [Inline Frame] UE4::SSE::FloorToInt32() | Engine/Runtime/Core/Public/Math/UnrealPlatformMathSSE.h:90 |
| 19 | 327 | 0.7886 | plugin | `anonymous namespace'::VF_ProjectNativePassageFloor() | Plugin/Private/VoxelStrateManager.cpp:471 |
| 20 | 298 | 0.7186 | engine | [Inline Frame] UE::Math::TVector<double>::operator*() | Engine/Runtime/Core/Public/Math/Vector.h:316 |
| 21 | 274 | 0.6608 | plugin | VoxelDensityProfile::FScopedTimer::~FScopedTimer() | Plugin/Private/VoxelDensityProfile.cpp:969 |
| 22 | 251 | 0.6053 | plugin | [Inline Frame] ?A0x7db52731::VF_ProjectTunnelSegmentXY() | Plugin/Private/VoxelCaveMorphology.cpp:955 |
| 23 | 240 | 0.5788 | plugin | ``anonymous namespace'::VF_EvaluateSweptTunnelChain'::`7'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:1056 |
| 24 | 220 | 0.5305 | plugin | [Inline Frame] ?A0x7db52731::VF_DistanceSquaredToAabb() | Plugin/Private/VoxelCaveMorphology.cpp:68 |
| 25 | 218 | 0.5257 | plugin | ``anonymous namespace'::VF_EvaluateSweptTunnelChain'::`7'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:1058 |
| 26 | 217 | 0.5233 | engine | [Inline Frame] FGenericPlatformMath::Max() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:968 |
| 27 | 213 | 0.5137 | plugin | VF_EvaluatePassageLandingSDF() | Plugin/Private/VoxelCaveMorphology.cpp:4394 |
| 28 | 205 | 0.4944 | plugin | VoxelDensityProfile::FScopedTimer::~FScopedTimer() | Plugin/Private/VoxelDensityProfile.cpp:971 |
| 29 | 204 | 0.4920 | engine | [Inline Frame] FMath::Square() | Engine/Runtime/Core/Public/Math/UnrealMathUtility.h:580 |
| 30 | 204 | 0.4920 | engine | [Inline Frame] UE::Math::TVector<double>::operator-() | Engine/Runtime/Core/Public/Math/Vector.h:1573 |

### LOD1+ (coarse/background)

#### Exclusive functions (leaf)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 16080 | 17.9454 | unknown | <unknown> | unknown |
| 2 | 13179 | 14.7079 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:77 |
| 3 | 6983 | 7.7931 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:54 |
| 4 | 5204 | 5.8077 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2382 |
| 5 | 4419 | 4.9316 | plugin | [Inline Frame] VoxelNoise::Detail::Lerp() | Plugin/Public/VoxelNoise.h:87 |
| 6 | 2936 | 3.2766 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:68 |
| 7 | 2863 | 3.1951 | plugin | `anonymous namespace'::VF_EvaluateRoomLandingDensity() | Plugin/Private/VoxelCaveMorphology.cpp:2576 |
| 8 | 2677 | 2.9876 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:3650 |
| 9 | 2657 | 2.9652 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 10 | 2578 | 2.8771 | plugin | `anonymous namespace'::VF_BuildPlayerFitStencil() | Plugin/Private/VoxelCaveMorphology.cpp:2049 |
| 11 | 2281 | 2.5456 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:170 |
| 12 | 1629 | 1.8180 | plugin | [Inline Frame] ?A0x7db52731::VF_IsPointInsidePlayerCapsule() | Plugin/Private/VoxelCaveMorphology.cpp:1993 |
| 13 | 1203 | 1.3426 | plugin | `anonymous namespace'::VF_ValidatePlayerFitPose() | Plugin/Private/VoxelCaveMorphology.cpp:2207 |
| 14 | 1196 | 1.3347 | plugin | [Inline Frame] VoxelHash::Mix() | Plugin/Public/VoxelCaveMorphology.h:217 |
| 15 | 1123 | 1.2533 | engine | [Inline Frame] TArray<UE::Math::TIntPoint<int>,TSizedDefaultAllocator<32> >::Emplace() | Engine/Runtime/Core/Public/Containers/Array.h:2602 |
| 16 | 980 | 1.0937 | plugin | `VoxelCaveMorphology::BuildChunkCache'::`2'::<lambda_7>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:5047 |
| 17 | 825 | 0.9207 | plugin | UVoxelGenerator::GetDensityAt() | Plugin/Private/VoxelGenerator.cpp:1673 |
| 18 | 711 | 0.7935 | engine | [Inline Frame] UE::Math::TVector<double>::operator/() | Engine/Runtime/Core/Public/Math/Vector.h:1585 |
| 19 | 657 | 0.7332 | plugin | `anonymous namespace'::VF_ProjectNativePassageFloor() | Plugin/Private/VoxelStrateManager.cpp:452 |
| 20 | 592 | 0.6607 | engine | [Inline Frame] UE::Math::TVector2<double>::{ctor}() | Engine/Runtime/Core/Public/Math/Vector2D.h:837 |
| 21 | 578 | 0.6451 | plugin | [Inline Frame] VoxelNoise::Perlin3D() | Plugin/Public/VoxelNoise.h:98 |
| 22 | 526 | 0.5870 | plugin | `VoxelCaveMorphology::EvaluateTunnelCoreWorld'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:6313 |
| 23 | 513 | 0.5725 | plugin | VoxelCaveMorphology::BuildChunkCache() | Plugin/Private/VoxelCaveMorphology.cpp:5192 |
| 24 | 504 | 0.5625 | engine | [Inline Frame] UE4::SSE4::FloorToFloat() | Engine/Runtime/Core/Public/Math/UnrealPlatformMathSSE4.h:34 |
| 25 | 496 | 0.5535 | plugin | [Inline Frame] ?A0x7db52731::VF_GetFloorReliefBound() | Plugin/Private/VoxelCaveMorphology.cpp:296 |
| 26 | 482 | 0.5379 | engine | [Inline Frame] FGenericPlatformMath::Min() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:990 |
| 27 | 438 | 0.4888 | plugin | [Inline Frame] `anonymous-namespace'::VF_FindPlayerFitPointForRoom::__l2::<lambda_1>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:2629 |
| 28 | 430 | 0.4799 | engine | [Inline Frame] TArray<FBuildRoom,TSizedInlineAllocator<64,32,TSizedDefaultAllocator<32> > >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 29 | 425 | 0.4743 | engine | [Inline Frame] FGenericPlatformMath::Max() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:968 |
| 30 | 400 | 0.4464 | engine | [Inline Frame] FMicrosoftPlatformMathBase::IsFinite() | Engine/Runtime/Core/Public/Microsoft/MicrosoftPlatformMath.h:18 |

#### Inclusive functions (once per sample)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 89605 | 100.0000 | engine | [Inline Frame] Invoke() | Engine/Runtime/Core/Public/Templates/Invoke.h:47 |
| 2 | 89605 | 100.0000 | engine | [Inline Frame] LowLevelTasks::FTask::Init::__l13::<lambda_1>::operator()() | Engine/Runtime/Core/Public/Async/Fundamental/Task.h:499 |
| 3 | 89605 | 100.0000 | engine | [Inline Frame] LowLevelTasks::TTaskDelegate<LowLevelTasks::FTask * __cdecl(bool),48>::TTaskDelegateImpl<`LowLevelTasks::FTask::Init<`UE::Tasks::Private::FTaskBase::Init'::`2'::<lambda_1> >'::`13'::<lambda_1>,0>::Call() | Engine/Runtime/Core/Public/Async/Fundamental/TaskDelegate.h:162 |
| 4 | 89605 | 100.0000 | engine | [Inline Frame] UE::Tasks::Private::FTaskBase::Init::__l2::<lambda_1>::operator()() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:180 |
| 5 | 89605 | 100.0000 | engine | LowLevelTasks::TTaskDelegate<LowLevelTasks::FTask * __cdecl(bool),48>::TTaskDelegateImpl<`LowLevelTasks::FTask::Init<`UE::Tasks::Private::FTaskBase::Init'::`2'::<lambda_1> >'::`13'::<lambda_1>,0>::CallAndMove() | Engine/Runtime/Core/Public/Async/Fundamental/TaskDelegate.h:171 |
| 6 | 89605 | 100.0000 | engine | UE::Tasks::Private::FTaskBase::TryExecuteTask() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:518 |
| 7 | 89605 | 100.0000 | engine | UE::Tasks::Private::TExecutableTaskBase<`AVoxelWorld::LoadTile'::`2'::<lambda_1>,void,void>::ExecuteTask() | Engine/Runtime/Core/Public/Tasks/TaskPrivate.h:898 |
| 8 | 89605 | 100.0000 | plugin | `AVoxelWorld::LoadTile'::`2'::<lambda_1>::operator()() | Plugin/Private/VoxelWorld.cpp:3766 |
| 9 | 89605 | 100.0000 | plugin | AVoxelWorld::GenerateTileResult() | Plugin/Private/VoxelWorld.cpp:4078 |
| 10 | 89569 | 99.9598 | plugin | UVoxelMarchingCubesMesher::GenerateMesh() | Plugin/Private/VoxelMarchingCubesMesher.cpp:574 |
| 11 | 89188 | 99.5346 | plugin | UVoxelGenerator::GetDensityAt() | Plugin/Private/VoxelGenerator.cpp:2073 |
| 12 | 81485 | 90.9380 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2800 |
| 13 | 72096 | 80.4598 | plugin | VoxelCaveMorphology::BuildChunkCache() | Plugin/Private/VoxelCaveMorphology.cpp:4974 |
| 14 | 66315 | 74.0081 | plugin | [Inline Frame] VoxelCaveMorphology::BuildChunkCache::__l2::<lambda_9>::operator()() | Plugin/Private/VoxelCaveMorphology.cpp:5153 |
| 15 | 66312 | 74.0048 | plugin | `anonymous namespace'::VF_FindPlayerFitPointForRoom() | Plugin/Private/VoxelCaveMorphology.cpp:2687 |
| 16 | 65715 | 73.3385 | plugin | `anonymous namespace'::VF_ValidatePlayerFitPose() | Plugin/Private/VoxelCaveMorphology.cpp:2221 |
| 17 | 47401 | 52.8999 | engine | [Inline Frame] UE::Core::Private::Function::TFunctionRefBase<UE::Core::Private::Function::FFunctionRefStoragePolicy,float __cdecl(float,float,float)>::operator()() | Engine/Runtime/Core/Public/Templates/Function.h:414 |
| 18 | 47272 | 52.7560 | engine | UE::Core::Private::Function::TFunctionRefCaller<``anonymous namespace'::VF_FindPlayerFitPointForRoom'::`2'::<lambda_1> const ,float,float,float,float>::Call() | Engine/Runtime/Core/Public/Templates/Function.h:298 |
| 19 | 46764 | 52.1891 | plugin | `anonymous namespace'::VF_EvaluateRoomLandingDensity() | Plugin/Private/VoxelCaveMorphology.cpp:2577 |
| 20 | 34914 | 38.9643 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2377 |
| 21 | 28597 | 31.9145 | plugin | [Inline Frame] VoxelNoise::Perlin3D() | Plugin/Public/VoxelNoise.h:118 |
| 22 | 14149 | 15.7904 | plugin | `anonymous namespace'::VF_BuildPlayerFitStencil() | Plugin/Private/VoxelCaveMorphology.cpp:2049 |
| 23 | 13179 | 14.7079 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:77 |
| 24 | 8269 | 9.2283 | engine | UE::Core::Private::ReallocGrow1_DoAlloc_Tiny<3,TSizedHeapAllocator<32,FMemory>::ForAnyElementType>() | Engine/Runtime/Core/Public/Containers/Array.h:463 |
| 25 | 6983 | 7.7931 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:54 |
| 26 | 4419 | 4.9316 | plugin | [Inline Frame] VoxelNoise::Detail::Lerp() | Plugin/Public/VoxelNoise.h:87 |
| 27 | 2936 | 3.2766 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:68 |
| 28 | 2657 | 2.9652 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 29 | 2548 | 2.8436 | plugin | VoxelCaveMorphology::EvaluateSDFCached() | Plugin/Private/VoxelCaveMorphology.cpp:6222 |
| 30 | 2426 | 2.7074 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:170 |

#### Exclusive source lines (leaf source)

| # | Samples | % | Label | Function | Source |
|---:|---:|---:|---|---|---|
| 1 | 16097 | 17.9644 | unknown | <unknown> | unknown |
| 2 | 4419 | 4.9316 | plugin | [Inline Frame] VoxelNoise::Detail::Lerp() | Plugin/Public/VoxelNoise.h:87 |
| 3 | 2936 | 3.2766 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:77 |
| 4 | 2803 | 3.1282 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:83 |
| 5 | 2614 | 2.9172 | engine | [Inline Frame] FGenericPlatformMath::Sqrt() | Engine/Runtime/Core/Public/GenericPlatform/GenericPlatformMath.h:553 |
| 6 | 2475 | 2.7621 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:82 |
| 7 | 2453 | 2.7376 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:59 |
| 8 | 2374 | 2.6494 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:80 |
| 9 | 1991 | 2.2220 | plugin | [Inline Frame] VoxelSDF::SmoothMin() | Plugin/Public/VoxelCaveMorphology.h:170 |
| 10 | 1822 | 2.0334 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2382 |
| 11 | 1789 | 1.9965 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:68 |
| 12 | 1773 | 1.9787 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:58 |
| 13 | 1570 | 1.7521 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:54 |
| 14 | 1567 | 1.7488 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:79 |
| 15 | 1364 | 1.5222 | engine | [Inline Frame] TArray<FBuildRoom,TSizedInlineAllocator<64,32,TSizedDefaultAllocator<32> > >::RangeCheck() | Engine/Runtime/Core/Public/Containers/Array.h:1095 |
| 16 | 1269 | 1.4162 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2377 |
| 17 | 1187 | 1.3247 | plugin | [Inline Frame] VoxelNoise::Detail::HashCorner() | Plugin/Public/VoxelNoise.h:57 |
| 18 | 1184 | 1.3214 | plugin | `anonymous namespace'::VF_ApplyCaveWarp() | Plugin/Private/VoxelCaveMorphology.cpp:2392 |
| 19 | 1180 | 1.3169 | plugin | `anonymous namespace'::VF_BuildPlayerFitStencil() | Plugin/Private/VoxelCaveMorphology.cpp:2049 |
| 20 | 1045 | 1.1662 | plugin | [Inline Frame] VoxelNoise::Detail::Fade() | Plugin/Public/VoxelNoise.h:67 |
| 21 | 1013 | 1.1305 | engine | [Inline Frame] TArray<UE::Math::TIntPoint<int>,TSizedDefaultAllocator<32> >::Emplace() | Engine/Runtime/Core/Public/Containers/Array.h:2602 |
| 22 | 998 | 1.1138 | plugin | [Inline Frame] VoxelNoise::Detail::GradDot() | Plugin/Public/VoxelNoise.h:84 |
| 23 | 997 | 1.1127 | plugin | [Inline Frame] ?A0x7db52731::VF_IsPointInsidePlayerCapsule() | Plugin/Private/VoxelCaveMorphology.cpp:1993 |
| 24 | 754 | 0.8415 | plugin | `anonymous namespace'::VF_EvaluateRoomLandingDensity() | Plugin/Private/VoxelCaveMorphology.cpp:2509 |
| 25 | 711 | 0.7935 | engine | [Inline Frame] UE::Math::TVector<double>::operator/() | Engine/Runtime/Core/Public/Math/Vector.h:1585 |
| 26 | 628 | 0.7009 | plugin | [Inline Frame] ?A0x7db52731::VF_IsPointInsidePlayerCapsule() | Plugin/Private/VoxelCaveMorphology.cpp:1995 |
| 27 | 592 | 0.6607 | engine | [Inline Frame] UE::Math::TVector2<double>::{ctor}() | Engine/Runtime/Core/Public/Math/Vector2D.h:837 |
| 28 | 564 | 0.6294 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2644 |
| 29 | 559 | 0.6238 | plugin | UVoxelGenerator::GetDensityWithParams() | Plugin/Private/VoxelGenerator.cpp:2648 |
| 30 | 504 | 0.5625 | engine | [Inline Frame] UE4::SSE4::FloorToFloat() | Engine/Runtime/Core/Public/Math/UnrealPlatformMathSSE4.h:34 |


## Component mapping

The percentages below are inclusive unless marked exclusive/source-line. They are LOD0 shares where the LOD0 row is available; all-LOD figures are called out for work dominated by coarse tiles. Inclusive values overlap and must not be added.

| Component | Static | Moving | Reading |
|---|---:|---:|---|
| Fused evaluator core, GetDensityWithParams | 55.38% LOD0 inclusive; 90.36% all-LOD | 58.67% LOD0 inclusive; 80.73% all-LOD | The evaluator is the parent of most density work. |
| Detail noise | GradDot 14.81%, HashCorner 7.67%, Lerp 4.87%, Fade 3.13% all-LOD exclusive leaves | GradDot 14.71%, HashCorner 7.79%, Lerp 4.93%, Fade 3.28% all-LOD exclusive leaves | Roughly 30% across these four resolved leaves before other noise helpers; this is a real arithmetic cost, not a wait frame. |
| Swept tunnel shape | VF_EvaluateSweptTunnel 11.56%; chain 10.99% LOD0 inclusive | 12.41%; chain 11.91% LOD0 inclusive | The chain is nested, so do not sum the two rows. |
| Tunnel floor projection | VF_ProjectNativePassageFloor 11.41%; VF_ProjectPassageFloor 7.45% LOD0 inclusive | 8.95%; 5.89% LOD0 inclusive | The current symbol names are the native/passage-floor implementations of the requested swept-floor component. |
| Tunnel core | EvaluateTunnelCoreWorld 12.39% and its lambda 11.83% LOD0 inclusive | 12.93% and 12.49% LOD0 inclusive | The core path is independently hot; the relevant implementation is around VoxelCaveMorphology.cpp:6275 and the resolved body at the later lambda lines. |
| Room graph / player fit | BuildChunkCache 79.54%, FindPlayerFitPointForRoom 73.35%, room landing 51.87% all-LOD inclusive | 80.46%, 74.00%, 52.19% all-LOD inclusive | This is mostly LOD1+ background work; LOD0's hot list shifts toward density/tunnel/floor work. |
| Candidate traversal | DistanceSquaredToAabb 1.53% LOD0 exclusive; floor-relief bound is 0.59% in LOD1+ | 2.18% LOD0 exclusive; floor-relief bound is 0.55% in LOD1+ | Cheap compared with tunnel/core evaluation. |
| Disturbances | ApplyDisturbances 0.7598% LOD0 exclusive | 0.9574% LOD0 exclusive | Not a top cost. |
| Post-stack structural tail | ApplyPassageStructuralPostsMC 14.47%, carving 14.07% LOD0 inclusive | 11.36%, carving 11.25% LOD0 inclusive | This is a material LOD0 tail after the density stack. |
| Surface column | FSurfaceColumn constructor 11.6522% LOD0 exclusive/source line; source file unresolved | 12.0674% LOD0 exclusive/source line; source file unresolved | The symbol is known but the source location is not; keep it as an unknown-source cost until PDB/source mapping is improved. |
| Per-sample FStrateGenerationParams copy | No isolated top-30 function or source-line share | No isolated top-30 function or source-line share | The ~74-field copy noted at VoxelDensityOpStack.cpp:3946 is folded into its caller or below the sampled leaf threshold; this profile cannot claim it is free or expensive. |
| Mesher | GenerateMesh 99.8692% LOD0 inclusive | 99.8770% LOD0 inclusive | This is the registered task parent, not an additive exclusive cost. Density is inside it. |

The old component names cover the main LOD0 path, but the profile adds two important named costs: room-cache/player-fit/landing work for coarse tiles and FSurfaceColumn construction in LOD0. Those are the clearest candidates for the cost that the old 34f06df scope view did not name.

## Hypotheses

- Swept tunnel evaluated twice: supported. The LOD0 stack repeatedly contains both VF_EvaluateSweptTunnel and EvaluateTunnelCoreWorld at roughly 11.6–12.9% inclusive each, and the existing counters report the tunnel and tunnel-core candidate populations both at 28,951,420. This is measured duplication evidence; proving that every candidate takes both full paths still requires a targeted call counter.
- Candidate rejection cheap, evaluation expensive: held. AABB distance is about 1.5–2.2% exclusive in LOD0, while swept-tunnel and tunnel-core bodies are each about 11.6–12.9% inclusive. The scan is not the 4.5x-sized cost.
- Per-sample parameter copy is a standalone cost: not proven. It never appears as an isolated top-30 function/source line in either headline run.
- A cost absent from the old named list exists: supported, but not uniquely differential. Room graph/player fit/landing dominates LOD1+ and FSurfaceColumn is a material LOD0 leaf. The sampler cannot by itself prove which of those was absent from 34f06df without profiling that old build.
- Static and moving game profiles differ materially. Static retained samples were 12.2% LOD0 and 87.8% LOD1+; moving was 31.6% LOD0 and 68.4% LOD1+. Moving therefore exposes more floor/tunnel/core/structural work, while static is more heavily diluted by coarse room-graph work.

## Planted-cost proof

Before trusting the sampler, a diagnostic-only 20000 us busy loop was planted in one function, VF_SampleStacksPlantedBusyLoop, for a sampler-on commandlet run. The summary retained 712 samples with 712 registered samples and no reservoir drops. The planted function was:

- exclusive: rank 21, 8 samples, 1.1236%;
- inclusive: rank 20, 75 samples, 10.5337%.

That is the expected approximately 10–12% share for the planted duration in the active worker interval. The plant was then removed; a source search for SampleStacksPlant and PlantedBusyLoop is clean.

## Secondary export cross-check

The owner redirected this round to the game path, so no further export tables were run. The already retained primary export cross-check remains:

- sampler-off mesh times: 0.971 s, 0.992 s, 0.954 s; mean 0.972333 s;
- sampler-on mesh times: 0.951 s, 0.964 s, 0.967 s; mean 0.960667 s;
- all off/on OBJ SHA-256 values: b3e5f4c398dd0547c6c55f415872058246d4a292c2f0d636ee1cd0829de9b377.

## Ranked fixes for the game path

These are measurement-derived estimates, not changes made this round. The ranges are expected LOD0 request-to-ready p95 and worker-CPU gains, assuming the relevant work can be removed or reused without changing scheduling.

1. Share one exact swept-tunnel result between the SDF path and EvaluateTunnelCoreWorld/floor metadata: expected LOD0 p95 gain 8–15%, worker CPU gain 8–15%. Classification: field-preserving only if the exact existing values and evaluation order are reused; otherwise field-changing. Capability risk is medium because tunnel floor/core metadata affects player-fit and walk reachability, so recheck player-fit 20,830 and walk-reachable 10,909.
2. Reuse or move deterministic room-cache/player-fit work out of the LOD0 request critical path: expected LOD0 p95 gain 3–8%, worker CPU gain 5–12%. Classification: field-preserving if cache keys include seed, generator parameters, room graph, and LOD and the returned values are copied unchanged; field-changing if candidate pruning or fit thresholds change. Capability risk is medium, concentrated in player-fit and walk-reachable coverage.
3. Reuse exact detail-noise values within one sample/evaluator invocation: expected LOD0 p95 gain 4–10%, worker CPU gain 8–18%. Classification: field-preserving for exact value reuse; any approximation, altered hash, or reordered floating-point expression is field-changing. Capability risk is high for an approximation because it can alter narrow tunnel openings and walk reachability; exact reuse carries low field risk.

No rank is assigned to the 74-field parameter copy: the sampler did not measure a standalone share, so optimizing it now would repeat the earlier fiction problem.

## Handoff

This round changed only diagnostic instrumentation and report artifacts under E:\Projet Unreal\VoxelM\Plugins\VoxelForge. Engine source was not modified. No commit, push, or stash was performed. The key caveat is explicit: the instrument has good closure and a successful planted-cost proof, but its 1 ms game overhead is above the requested timing/worker-CPU noise on static and above timing noise on moving. Treat the large, repeated component differences as useful; do not treat small single-digit improvements as established until that overhead is brought inside the game baseline.

## Floor round 1 — field-preserving LOD0 work (2026-09-13)

This section is the final report for the subsequent LOD0 floor round. It is deliberately separate
from the field-changing world-space tunnel-core merge and cave warp work. No density expression was
changed, no terrain-op order was changed, and no engine source was modified.

### Changes

- Passage floor projection is memoised in `FPassageEvaluationCache::FloorProjections`, one indexed
  entry per passage and worker. Native projection, generic fallback projection, and walkable-air
  classification reuse the same float outputs for an exact `(sample, passage)` hit. The sample key
  compares the original double XYZ values; cache invalidation includes manager lifetime, layout
  version, and chunk. `voxel.FloorRound1PassageProjectionCache` disables only this cache for A/B.
- Disabled `VoxelDensityProfile::FScopedTimer` construction now performs the enabled/probe test and
  carries only an inactive flag; bucket/token initialization and out-of-line destructor work are
  absent. Enabled profiling still enters/exits the same scope and preserves `End()` early closure.
- Proven-hot room/tunnel/passage candidate loops cache counts and use `GetData()` pointers where the
  preceding count or spatial-index range proves the access. Other array checks remain intact.
- The two exact floor-projection XY loops scalarise the existing double `TVector2` temporaries while
  retaining the original float conversions and result casts. `FVector::DistSquared` float paths were
  left alone.
- `FSurfaceColumn` is now a trivial payload. `ComputeSurfaceColumn` writes all five values before
  publishing `Computed`, so removing never-read default values removes its generated TLS constructor
  leaf without changing a column or density.

### Build and field/capability gates

The prescribed UBT build succeeded on the staged host. The loaded runtime was verified on every field,
capability, and game process as:

`E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Binaries\Win64\UnrealEditor-VoxelForge.dll`

SHA-256: `E54290C5056D63A12E407E39EED96935CCDC87F82882E9A607C84759DF8656D7`.

Field runs:

- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\FloorR1BookField2Off_20260913Out`
- `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\FloorR1BookField2On_20260913Out`

Both reports are `status=ok`, deterministic repeat `passed`, geometry CRC `07C14005`, 82,273
vertices, and 153,346 triangles. Both `geometry.obj` files are byte-identical with SHA-256
`b3e5f4c398dd0547c6c55f415872058246d4a292c2f0d636ee1cd0829de9b377`. The standing 841/841
per-tile triangle-identity record is preserved; the new game runs also report identical static and
moving applied tile/visible-tile/triangle totals in every off/on case.

Capability run:

`E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\FloorR1BookCapability_20260913Out\explore.json`

The report is `status=ok`, deterministic repeat `passed`, with `player_fit_volume_cells=20830`,
`reachable_player_fit_cells=10909`, and `walk_only_reachable_player_fit_cells=10909`.

### Clean game A/B

Sampler off, diagnostics off, LOD0, `voxel.OuterClassifierMode=0`, interleaved in the order
off/on static A, on/off moving A, off/on static B, on/off moving B. Static runs had 343 LOD0 ready
samples; moving runs had 1,274.

| run | cache | request p50/p95 (s) | generation p50/p95 (s) | worker s | applied tiles / visible / triangles |
|---|---:|---:|---:|---:|---:|
| OffStaticA | 0 | 0.091413 / 0.145960 | 0.066545 / 0.108736 | 33.354203 | 841 / 447 / 669834 |
| OnStaticA | 1 | 0.091688 / 0.151787 | 0.066298 / 0.107866 | 33.549964 | 841 / 447 / 669834 |
| OnMovingA | 1 | 0.067281 / 0.138041 | 0.059686 / 0.108387 | 97.919700 | 2348 / 1055 / 1985216 |
| OffMovingA | 0 | 0.067659 / 0.135287 | 0.059316 / 0.107414 | 98.007684 | 2348 / 1055 / 1985216 |
| OffStaticB | 0 | 0.088911 / 0.146950 | 0.064988 / 0.104965 | 33.246204 | 841 / 447 / 669834 |
| OnStaticB | 1 | 0.090368 / 0.151585 | 0.065554 / 0.107661 | 33.323367 | 841 / 447 / 669834 |
| OnMovingB | 1 | 0.067540 / 0.139517 | 0.059545 / 0.108946 | 97.915109 | 2348 / 1055 / 1985216 |
| OffMovingB | 0 | 0.067745 / 0.137666 | 0.059707 / 0.108898 | 97.926095 | 2348 / 1055 / 1985216 |

Paired means, on relative to off:

| mode | off request p50/p95 | on request p50/p95 | off generation p50/p95 | on generation p50/p95 | off worker s | on worker s | on vs off worker |
|---|---:|---:|---:|---:|---:|---:|---:|
| static | 0.090162 / 0.146455 | 0.091028 / 0.151686 | 0.065767 / 0.106851 | 0.065926 / 0.107764 | 33.300204 | 33.436666 | +0.41% |
| moving | 0.067702 / 0.136477 | 0.067411 / 0.138779 | 0.059512 / 0.108156 | 0.059616 / 0.108667 | 97.966890 | 97.917405 | −0.05% |

The isolated projection-cache A/B is therefore neutral within run-to-run noise (an earlier brief
blamed cross-round drift on "daytime use"; that was false, the machine was idle): static is
slightly worse, moving worker sum is effectively unchanged. The implementation is retained because
the reuse is exact and the switch attributes the hypothesis; the result is not claimed as a speed
win. All eight runs had `validation_density_calls=0`, `obsolete_worker_tasks=0`, and loaded-DLL
verification true. The one-second process monitor observed game RSS from 1,946,759,168 to
2,019,475,456 bytes; these are peak samples, not a cross-round memory baseline.

### Final sampler-on moving run

Raw samples:

`E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\VoxelStackSamples_game_3352.csv`

Summary:

`E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\VoxelStackSummary_game_3352.txt`

The run retained 102,854 samples, dropped 0, had 67,187 LOD0 samples, ran 24.978248 s, and had
unknown-frame fraction 0.472158. It reported 2,348 applied tiles, 1,055 visible tiles, 1,985,216
triangles, 2,360 generation tasks, 103.016440 worker seconds, and zero obsolete worker tasks.

LOD0 exclusive top 20:

| # | samples | % | leaf |
|---:|---:|---:|---|
| 1 | 11531 | 17.1625 | `GetDensityWithParams` — `VoxelGenerator.cpp:2874` |
| 2 | 8658 | 12.8864 | unknown |
| 3 | 3068 | 4.5664 | `GetDensityAt` — `VoxelGenerator.cpp:1998` |
| 4 | 2702 | 4.0216 | `Sqrt` |
| 5 | 2312 | 3.4411 | `VoxelSDF::SmoothMin` |
| 6 | 2209 | 3.2878 | `EvaluateTunnelCoreWorld` lambda — `VoxelCaveMorphology.cpp:7648` |
| 7 | 1876 | 2.7922 | `EvaluateSDFCached` lambda — `VoxelCaveMorphology.cpp:7526` |
| 8 | 1788 | 2.6612 | `VF_ProjectNativePassageFloorUncached` — `VoxelStrateManager.cpp:597` |
| 9 | 1504 | 2.2385 | `VF_DistanceSquaredToAabb` — `VoxelCaveMorphology.cpp:68` |
| 10 | 1419 | 2.1120 | `VF_ProjectTunnelSegmentXY` — `VoxelCaveMorphology.cpp:970` |
| 11 | 1268 | 1.8873 | `ProjectWalkableTunnelFloor` — `VoxelPassageGeometry.h:314` |
| 12 | 1241 | 1.8471 | `VF_EvaluateSweptTunnelChain` — `VoxelCaveMorphology.cpp:1006` |
| 13 | 1009 | 1.5018 | swept-chain lambda — `VoxelCaveMorphology.cpp:1076` |
| 14 | 880 | 1.3098 | `VF_EvaluatePassageLandingSDF` — `VoxelCaveMorphology.cpp:5690` |
| 15 | 844 | 1.2562 | `GenerateMesh` — `VoxelMarchingCubesMesher.cpp:578` |
| 16 | 758 | 1.1282 | `FScopedTimer::End` — `VoxelDensityProfile.h:432` |
| 17 | 732 | 1.0895 | `EvaluateModifierSDF` — `VoxelStrateManager.cpp:2470` |
| 18 | 729 | 1.0850 | `ApplyPassageStructuralPostsMC` — `VoxelStrateManager.cpp:2808` |
| 19 | 700 | 1.0419 | `VF_GetPassageFloorProjectionCacheEntry` — `VoxelStrateManager.cpp:349` |
| 20 | 609 | 0.9064 | `EvaluateSDFCached` lambda — `VoxelCaveMorphology.cpp:7397` |

LOD0 inclusive top 20:

| # | samples | % | inclusive function |
|---:|---:|---:|---|
| 1 | 67187 | 100.0000 | `Invoke` |
| 2 | 67187 | 100.0000 | `LowLevelTasks::FTask::Init` lambda |
| 3 | 67187 | 100.0000 | `LowLevelTasks::TTaskDelegate::Call` |
| 4 | 67187 | 100.0000 | `UE::Tasks::Private::FTaskBase::Init` lambda |
| 5 | 67187 | 100.0000 | `LowLevelTasks::TTaskDelegate::CallAndMove` |
| 6 | 67187 | 100.0000 | `UE::Tasks::Private::FTaskBase::TryExecuteTask` |
| 7 | 67187 | 100.0000 | `TExecutableTaskBase::ExecuteTask` |
| 8 | 67187 | 100.0000 | `AVoxelWorld::LoadTile` lambda — `VoxelWorld.cpp:3782` |
| 9 | 67186 | 99.9985 | `AVoxelWorld::GenerateTileResult` — `VoxelWorld.cpp:4126` |
| 10 | 67100 | 99.8705 | `UVoxelMarchingCubesMesher::GenerateMesh` — `VoxelMarchingCubesMesher.cpp:578` |
| 11 | 65522 | 97.5218 | `UVoxelGenerator::GetDensityAt` — `VoxelGenerator.cpp:2297` |
| 12 | 40538 | 60.3361 | `UVoxelGenerator::GetDensityWithParams` — `VoxelGenerator.cpp:3117` |
| 13 | 18270 | 27.1928 | `VF_ForEachSpatialCandidate` — `VoxelCaveMorphology.cpp:1582` |
| 14 | 11093 | 16.5106 | `VoxelCaveMorphology::EvaluateSDFCached` — `VoxelCaveMorphology.cpp:7377` |
| 15 | 8829 | 13.1409 | `EvaluateSDFCached` lambda — `VoxelCaveMorphology.cpp:7526` |
| 16 | 8777 | 13.0635 | `VF_EvaluateSweptTunnel` — `VoxelCaveMorphology.cpp:1167` |
| 17 | 8581 | 12.7718 | `VoxelCaveMorphology::EvaluateTunnelCoreWorld` — `VoxelCaveMorphology.cpp:7732` |
| 18 | 8366 | 12.4518 | `VF_EvaluateSweptTunnelChain` — `VoxelCaveMorphology.cpp:1029` |
| 19 | 7811 | 11.6258 | `ApplyPassageCarvingOnly` — `VoxelStrateManager.cpp:2596` |
| 20 | 7682 | 11.4338 | `VF_ApplyPassageCarving` — `VoxelDensityPrimitives.h:75` |

### Unknown-source resolution

The old `FSurfaceColumn` constructor PCs are absent from this final sampler. The LOD0 unknown leaf
is 8,658 samples (12.8864%), but the raw PC cluster that accounts for the largest part is not plugin
code: `0x00007FF88382C080`, `...C08A`, `...C095`, `...C0A5`, and `...C0A9` map to the loaded
`UnrealEditor-Core.dll` at runtime RVA `0xC080`, inside its private `.pdata` function range
`0xB4D0–0xC32B`. The packaged Core binary has no PDB, so the source function name cannot be resolved.
Smaller unresolved groups map to `UnrealEditor-Engine.dll + RVA 0x500947` (generated reflection
`StaticStruct` code) and `UnrealEditor-CoreUObject.dll + RVA 0x76168`; none maps to
`Plugins\VoxelForge`.

This closes the actionable part of candidate 5: the previous plugin constructor was removed, while
the remaining unknown is an engine-symbolisation limitation. No commit, push, or stash was performed.

## Worm block skip round (2026-09-14)

### Proof and gates

The worm skip is enabled by default through `voxel.WormBlockSkip=1`. It is fail-closed and only
removes the two worm noise calls after the unchanged `WormNetworkRange` mask has passed. The
worker-local cache is keyed by generator owner, seed, params/layout fingerprint, tile origin,
step, lattice size, and worm parameters; it is never shared across tiles or workers.

For this implementation's actual `GradDot` set, the unique gradients are
`(±1,±1,0)`, `(±1,0,±1)`, and `(0,±1,±1)`. The exact finite-support partial-derivative relaxation
with `Fade'(t)=30t²(1-t)²` has supremum `15/4`, attained at `(0.5,0.5,0.5)`. Therefore the
noise-space vector bound is

`L = (15/4) * sqrt(3) * VOXEL_NOISE_SCALE = 8.118988160` (`VOXEL_NOISE_SCALE=1.25`).

The existing 2D floor constant was proven differently and does not transfer: its comment uses
the loose component bound `[-1,1]`, four-edge interpolation difference bound, and
`max SmoothCurve' = 1.875`, giving `1 + 4*1.875 = 8.5` per partial. The worm derives its
bound from the actual 3D gradient support. The block radius is measured after the exact X/Y/Z
noise transform, including `WormFrequency`, `VerticalScale`, and `WormHorizontalBias`; `Abs` is
1-Lipschitz and the positive noise scale is included in `L`. A `1e-3` scaled-output margin is
subtracted before the threshold comparison. It is deliberately much larger than the accumulated
float rounding at the center/radius operations and is covered by the soundness test.

The adversarial attack used dense corner/face sampling plus random pairs: 17,365,280 pairs,
worst scaled ratio `3.187330411`, or `0.392577295*L`. The fail-capable automation test also
audited 6,476 proved blocks over 355,945 exact lattice samples with zero violations. A deliberately
halved `L` proves the negative-control block while the exact point has `N1=0.011259466 < 0.015`
and the full `L` rejects it.

Default-on export is byte-identical to the baseline: geometry hash
`B3E5F4C398DD0547C6C55F415872058246D4A292C2F0D636EE1CD0829DE9B377`, 82,273 vertices,
153,346 triangles. The static trace is 841/841 keyed tiles with zero field/empty/triangle
differences. Capability remains 20,830 / 10,909.

### Clean game A/B

Sampler off, diagnostics off, `voxel.OuterClassifierMode=0`, interleaved static and moving runs.
Static runs had 343 LOD0 ready samples; moving runs had 1,274. All valid runs had identical
applied geometry totals, zero validation calls, zero obsolete worker tasks, and verified the loaded
plugin module. The clean timings below used the staged runtime before the final counter-only
diagnostic-accounting rebuild (`81F52389A78543E41EEC8E3E40DEF8F2634A79EB83E2A933FF35EE42CD611E12`).
That patch is behind diagnostics, so it does not change this clean path. The final staged runtime
used by the last diagnostic run is `E72E140100B3D258F28E9DBA19CD95D379D98794DA49D24C708FDD01B5B5FE08`.

| run | switch | request p50/p95 (s) | generation p50/p95 (s) | worker s |
|---|---|---:|---:|---:|
| WormSkipGameOffStaticA_20260914 | off | 0.087151 / 0.150116 | 0.063115 / 0.104426 | 33.308914 |
| WormSkipGameOnStaticA_Resume_20260914 | on | 0.092174 / 0.154458 | 0.065730 / 0.110507 | 34.980771 |
| WormSkipGameOffMovingA_20260914 | off | 0.066421 / 0.138179 | 0.059918 / 0.108181 | 99.193222 |
| WormSkipGameOnMovingA_20260914 | on | 0.064163 / 0.135519 | 0.059273 / 0.106991 | 97.624292 |
| WormSkipGameOffStaticB_20260914 | off | 0.090372 / 0.160169 | 0.064016 / 0.109362 | 34.116813 |
| WormSkipGameOnStaticB_Resume_20260914 | on | 0.091922 / 0.154718 | 0.063731 / 0.112871 | 33.892686 |
| WormSkipGameOffMovingB_Resume_20260914 | off | 0.066659 / 0.142168 | 0.061916 / 0.110043 | 100.995082 |
| WormSkipGameOnMovingB_Resume_20260914 | on | 0.065557 / 0.141592 | 0.060515 / 0.110629 | 99.716139 |

Paired means, on relative to off:

| mode | request p50/p95 | generation p50/p95 | worker s | on vs off worker |
|---|---:|---:|---:|---:|
| static | off 0.088762 / 0.155143; on 0.092048 / 0.154588 | off 0.063566 / 0.106894; on 0.064731 / 0.111689 | 33.712864 → 34.436729 | +2.1471% |
| moving | off 0.066540 / 0.140174; on 0.064860 / 0.138556 | off 0.060917 / 0.109112; on 0.059894 / 0.108810 | 100.094152 → 98.670216 | −1.4226% |

The two-run result is mixed: moving improves modestly, while static is within ordinary run noise
and is slightly worse on worker sum. The switch remains default-on because the field proof is exact;
the timing result is not presented as a universal speed claim.

### Skip rate

The final diagnostics-enabled moving run used the final DLL and emitted 2,360 tile profiles. The
rate is `worm_block_skipped / worm_eligible`, not a fraction of all density samples:

| LOD | worm-eligible samples | successful block proofs | skipped eligible samples | skip rate |
|---:|---:|---:|---:|---:|
| 0 | 12,381,885 | 9,541 | 503,696 | 4.068007% |
| 1 | 4,821,000 | 0 | 0 | 0.000000% |
| 2 | 188,063 | 0 | 0 | 0.000000% |
| 3 | 58,276 | 0 | 0 | 0.000000% |
| 4 | 25,253 | 0 | 0 | 0.000000% |

### Sampler-on moving profile

`WormSkipGameOnMovingSampler_20260914` retained 96,649 samples with zero drops and zero capture
failures: 61,869 LOD0 and 34,780 LOD1+. It ran for 24.972305 s; resolved-frame fraction was
52.9174%, unknown-frame fraction 47.0826%, wait-like fraction 0.1011%, and resolved-leaf
fraction 95.3388%.

LOD0 exclusive top 20:

| # | samples | % | leaf |
|---:|---:|---:|---|
| 1 | 12472 | 20.1587 | `GetDensityWithParams` — `VoxelGenerator.cpp:3128` |
| 2 | 3137 | 5.0704 | `GetDensityAt` — `VoxelGenerator.cpp:2994` |
| 3 | 2853 | 4.6114 | unknown |
| 4 | 2540 | 4.1054 | `Sqrt` |
| 5 | 2238 | 3.6173 | `VoxelSDF::SmoothMin` — `VoxelCaveMorphology.h:170` |
| 6 | 1736 | 2.8059 | `VF_ProjectNativePassageFloorUncached` — `VoxelStrateManager.cpp:561` |
| 7 | 1704 | 2.7542 | `FChunkSDFSpatialIndex::GetRange` — `VoxelCaveMorphology.cpp:1356` |
| 8 | 1434 | 2.3178 | `VF_ProjectTunnelSegmentXY` — `VoxelCaveMorphology.cpp:970` |
| 9 | 1244 | 2.0107 | `ProjectWalkableTunnelFloor` — `VoxelPassageGeometry.h:338` |
| 10 | 1229 | 1.9865 | `VF_EvaluateSweptTunnelChain` — `VoxelCaveMorphology.cpp:1128` |
| 11 | 1151 | 1.8604 | `EvaluateTunnelCoreWorld` lambda — `VoxelCaveMorphology.cpp:7635` |
| 12 | 1009 | 1.6309 | swept-chain lambda — `VoxelCaveMorphology.cpp:1074` |
| 13 | 938 | 1.5161 | `EvaluateSDFCached` lambda — `VoxelCaveMorphology.cpp:7526` |
| 14 | 819 | 1.3238 | `IsFiniteFast` — `VoxelTypes.h:30` |
| 15 | 769 | 1.2429 | `IsFinite` — `VoxelTypes.h:47` |
| 16 | 764 | 1.2349 | `EvaluateModifierSDF` — `VoxelStrateManager.cpp:2451` |
| 17 | 748 | 1.2090 | `VF_EvaluatePassageLandingSDF` — `VoxelCaveMorphology.cpp:5673` |
| 18 | 739 | 1.1945 | `GenerateMesh` — `VoxelMarchingCubesMesher.cpp:578` |
| 19 | 724 | 1.1702 | `FScopedTimer::End` — `VoxelDensityProfile.h:435` |
| 20 | 700 | 1.1314 | `VF_DistanceSquaredToAabb` — `VoxelCaveMorphology.cpp:74` |

LOD0 inclusive top 20:

| # | samples | % | inclusive function |
|---:|---:|---:|---|
| 1 | 61869 | 100.0000 | `Invoke` |
| 2 | 61869 | 100.0000 | `LowLevelTasks::FTask::Init` lambda |
| 3 | 61869 | 100.0000 | `LowLevelTasks::TTaskDelegate::Call` |
| 4 | 61869 | 100.0000 | `UE::Tasks::Private::FTaskBase::Init` lambda |
| 5 | 61869 | 100.0000 | `LowLevelTasks::TTaskDelegate::CallAndMove` |
| 6 | 61869 | 100.0000 | `UE::Tasks::Private::FTaskBase::TryExecuteTask` |
| 7 | 61869 | 100.0000 | `TExecutableTaskBase::ExecuteTask` |
| 8 | 61869 | 100.0000 | `AVoxelWorld::LoadTile` lambda — `VoxelWorld.cpp:3782` |
| 9 | 61869 | 100.0000 | `AVoxelWorld::GenerateTileResult` — `VoxelWorld.cpp:4142` |
| 10 | 61774 | 99.8464 | `GenerateMesh` — `VoxelMarchingCubesMesher.cpp:578` |
| 11 | 60310 | 97.4802 | `GetDensityAt` — `VoxelGenerator.cpp:2606` |
| 12 | 38073 | 61.5381 | `GetDensityWithParams` — `VoxelGenerator.cpp:3426` |
| 13 | 15666 | 25.3212 | `VF_ForEachSpatialCandidate` — `VoxelCaveMorphology.cpp:1570` |
| 14 | 9775 | 15.7995 | `EvaluateSDFCached` — `VoxelCaveMorphology.cpp:7484` |
| 15 | 7846 | 12.6816 | `VF_EvaluateSweptTunnel` — `VoxelCaveMorphology.cpp:1167` |
| 16 | 7460 | 12.0577 | `VF_EvaluateSweptTunnelChain` — `VoxelCaveMorphology.cpp:1128` |
| 17 | 7276 | 11.7603 | `ApplyPassageCarvingOnly` — `VoxelStrateManager.cpp:2596` |
| 18 | 7209 | 11.6520 | `EvaluateTunnelCoreWorld` — `VoxelCaveMorphology.cpp:7732` |
| 19 | 7128 | 11.5211 | `VF_ApplyPassageCarving` — `VoxelDensityPrimitives.h:75` |
| 20 | 7106 | 11.4856 | `EvaluateModifierSDF` — `VoxelStrateManager.cpp:2538` |

### Run ledger

Every worm-round launch is listed here, including invalidated launches and the compile retry:

| launch | outcome |
|---|---|
| Initial staged build | Failed compiling `VoxelForgeWormBlockSkipTest.cpp` because of an ambiguous `FMath::IsNearlyEqual` overload; fixed by an explicit cast. No Unreal process was launched. |
| Rebuilt staged plugin | Succeeded; final build log reports `Result: Succeeded`. |
| `VoxelForge.Determinism.WormBlockSkipSoundness` | Passed: 6,476 proofs / 355,945 lattice samples / 0 violations; halved-L control fails as intended. |
| `WormSkipFieldOn_20260914` | Passed; export byte-identical and loaded module verified. |
| `WormSkipStaticTrace_20260914` | Data passed 841/841; harness intentionally stopped the process after `.done` (`ExitCode=-1`), not a crash. |
| `WormSkipCapability_20260914` | Passed; capability 20,830 / 10,909. |
| `WormSkipGameOffStaticA_20260914` | Completed; valid off A. |
| `WormSkipGameOnStaticA_20260914` | Launched before the stop; void by owner instruction and excluded, even though a complete artifact exists. |
| `WormSkipGameOffMovingA_20260914` | Completed; valid on/off interleave partner. |
| `WormSkipGameOnMovingA_20260914` | Completed; valid on/off interleave partner. |
| `WormSkipGameOffStaticB_20260914` | Completed; valid off B. |
| `WormSkipGameOnStaticB_20260914` | Interrupted at the stop; partial module/RSS artifacts only, void and excluded. |
| `WormSkipGameOnStaticA_Resume_20260914` | Completed; valid on A replacement. |
| `WormSkipGameOffMovingB_Resume_20260914` | Completed; valid off B replacement. |
| `WormSkipGameOnMovingB_Resume_20260914` | Completed; valid on B replacement. |
| `WormSkipGameOnStaticB_Resume_20260914` | Completed; valid on B replacement. |
| `WormSkipGameOnMovingSampler_20260914` | Completed; sampler-on moving profile, zero drops/failures. |
| `WormSkipDiagnosticsMoving_20260914` | Completed; superseded by the final counter-semantics diagnostic run, not used for reported skip rates. |
| `WormSkipDiagnosticsMovingProofs_20260914` | Completed on final DLL; per-LOD skip rates above, module verified. |

The Saved crash inventory also contains earlier-round UECC artifacts, which are kept visible here:
three failed `Task4CurrentTracePIE` attempts on 2026-09-09 (plugin-manager/ShaderCore/DDC launch
failures), `GameMovingOn_20260913` (fatal shutdown), `MemoGameStaticOffA_20260913` (no writable
DDC graph; retried successfully), `TakeoverFinalMovingSamplerOnHost_20260913` (crash artifact;
`...Host2` retry succeeded), `Final3MovingSamplerOn_20260913` (fatal shutdown), and
`IsFiniteExact_20260913` (no writable DDC graph). No CrashReportClient crash occurred in the
worm-round launches above.

## Final worms round — real off switch, cheap field, and offline stacks (2026-09-14)

This addendum covers the implementation and the final hardening runs after the block-skip round
above. Existing historical measurements are retained. Scope stayed inside
`E:\Projet Unreal\VoxelM\Plugins\VoxelForge`; no `.uasset` content was edited and nothing was
committed, pushed, or stashed.

### Final build

The prescribed UE 5.7 UBT command in this report was used throughout. Three intermediate staged
builds failed before the final game runs: the first had stale-source compile errors (`Tunnel` not
found, missing `FGuid`, and the offline module record missing `PdbAge`); the second exposed a
linker error from editor access to unexported tile-context TLS; the third cleanup build exposed a
missing anonymous-namespace close after removing the in-process resolver. Each failure was fixed;
none left an Unreal process running.

The final hardening build succeeded in
`E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\PerfSampledBuild_FinalHardening.log`
(`Result: Succeeded`, 9.11 s). The subsequent benchmark-only force-off wiring rebuild also
succeeded in
`E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\PerfSampledBuild_FinalForceOff.log`
(`Result: Succeeded`, 81.89 s). The final off/on game runs below use that latter staged build.
Its loaded staged DLLs were verified as:

- runtime: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Binaries\Win64\UnrealEditor-VoxelForge.dll`, SHA-256 `5A79B6FBDE2944D66C762EDB2D099E634B2BE84C08808D48C8CB7A3897E233FD`;
- editor: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Binaries\Win64\UnrealEditor-VoxelForgeEditor.dll`, SHA-256 `AB20BE8A2CBAF5FA4D9D2545AA7CC9DC058085741EA18B9FEAB26669A92F9F77`.

After the performance runs, the cache-key-only hardening described below was rebuilt in
`E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\PerfSampledBuild_FinalFingerprint.log`
(`Result: Succeeded`, 8.87 s). Its runtime hash is
`19190CB64AB894097A49C37B8DC5D43B67018A1BD624253473D4F710FBECB532` (the editor hash is
unchanged). `WormFinalFingerprintSmoke_20260914` then loaded that exact staged runtime, used the
plugin DDC, and exited cleanly; the acceptance tables below remain measurements from the force-off
build above, while this smoke verifies the final source tree after the key hardening.

### Part 0 — module map and offline symbolizer

`FVoxelStackSampler::StopAndWrite` now joins the sampler first, writes the bounded raw CSV, then
enumerates the current process with PSAPI (`EnumProcessModules`, `GetModuleInformation`, and
`GetModuleFileNameExW`). The sibling
`VoxelStackModules_<label>_<pid>.tsv` records module path, load base, image size, file size, last
write FILETIME, and the CodeView PDB GUID/age/path when the loaded image exposes an RSDS record.
The image identity read is a bounded PE-header read; the measured process does not initialize or
call DbgHelp. The raw summary explicitly says `symbolization: deferred` and
`dbghelp_in_measured_process: no`.

`-run=VoxelForgeExplore -symbolizestacks=<csv>` is now an editor-only post-exit commandlet. It
loads the sibling module map, checks recorded file size and timestamp, maps each PC to module and
RVA, loads only the modules needed by the CSV into DbgHelp, resolves functions and source lines,
and queries inline frames. It writes a resolved-PC TSV and the former all-LOD, LOD0, and LOD1+
exclusive-function, inclusive-function, and source-line tables. DbgHelp is linked by
`VoxelForgeEditor` only.

The fresh final cheap moving sampler run was
`WormFinalForceOffBuildSamplerOnMoving_20260914`. It completed cleanly with 1,521 LOD0 samples,
request-to-ready p50/p95 `0.062287 / 0.135633 s`, generation p50/p95 `0.054268 / 0.106542 s`,
and `113.578722` worker seconds. Its artifacts are:

- raw CSV: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\VoxelStackSamples_game_26772.csv`;
- module map: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\VoxelStackModules_game_26772.tsv`;
- raw summary: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\VoxelStackSummary_game_26772.txt`;
- resolved PCs: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\VoxelStackResolved_game_26772.tsv`;
- offline summary: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\VoxelStackSummaryOffline_game_26772.txt`.

The offline pass completed with 733 recorded modules, 10 needed and loaded, zero identity
mismatches, 109,593 samples (70,968 LOD0 and 38,625 LOD1+), 1,879,380 frame slots, 993,473
resolved frames, 6,714 inline symbols, 7,795 mapped unique PCs, and zero unmapped PCs. The
unknown-frame count is 885,907 and is retained as an explicit table row. The exact moving proof
(`WormFinalMovingSamplerExactProof_20260914` plus its offline pass) independently completed with
zero identity mismatches as well. The post-exit retry was
`WormFinalForceOffBuildSamplerOfflineRetry_20260914`; its first direct attempt is recorded as a
failed launch below because it omitted the writable local DDC arguments.

The LOD0 exclusive identities in the fresh offline table match the old in-process exact
configuration: `GetDensityWithParams`, `GetDensityAt`, `Sqrt`, `SmoothMin`,
`FChunkSDFSpatialIndex::GetRange`, `VF_ProjectNativePassageFloorUncached`,
`VF_ProjectTunnelSegmentXY`, `VF_EvaluateSweptTunnelChain`, `EvaluateTunnelCoreWorld`, and
`ProjectWalkableTunnelFloor`. Counts differ because these are separate moving reservoirs; the
function/source identities and table shape are the comparison.

The planted-cost proof used `WormSamplerPlantGenerator_20260914` and
`WormSamplerPlantGeneratorOffline_20260914`. The post-exit map for `game_22152` contained five
resolved `VoxelWormField::RunSampleStacksPlant()` rows, all module identities matched, and all
planted PCs mapped. The two earlier plant attempts (`WormSamplerPlantOffline_20260914` and
`WormSamplerPlantLongOffline_20260914`) completed but were inconclusive because the hook was
placed where it was optimized away from the captured stack. The plant was then removed; a source
and harness search has no `RunSampleStacksPlant` or `SampleStacksPlant` match.

The final disabled sampler proof used `WormFinalOffSampler_20260914` followed by
`WormFinalOffSymbolize_20260914`. It produced 1,992 untagged commandlet samples (the commandlet
has no generation-task LOD tag) and zero resolved rows containing `Worm`, which is the required
zero-worm-frame result for the disabled configuration.

### Part 1 — asset-owned off switch

`UVoxelStrateDefinition` now has `bEnableWorms`, defaulting to `true`, so existing assets retain
their current behavior. It is deliberately not a member of `FStrateGenerationParams`; the
`VF_STRATE_PARAM_FIELDS` list and its static assertions remain at 83 fields plus 8 bytes of
padding. The manager resolves the asset switch after composer candidates, season vectors, and
boundary blending, and the generation-parameter fingerprint includes the switch. Composer
runtime overrides are also clamped before publication, so a roll cannot re-enable a disabled
asset.

`voxel.WormsForceOff=1` is present only as a development measurement override for the game
harness: `0` uses the asset-owned value and `1` forces all resolved worm strengths to zero. It is
resolved once per process, included in the generation fingerprint, and explicitly documented as
world-changing; it is not the per-strate switch and must never vary between multiplayer peers or
world regenerations.

With the resolved strength at zero, the fused evaluator does not enter the worm block or its
timer; the op-stack builder omits the worm source, an already-materialized source returns exact
identity and zero carve bounds, and the interval/classifier paths do no worm evaluation. The
same gate is applied to the recipe materializer and the composer override path.

The final post-hardening geometry proof is exact:

| configuration | geometry hash | triangles | player-fit | walk-reachable | connectivity |
|---|---:|---:|---:|---:|---|
| `WormPostHardeningOff_20260914` (`-wormsenabled=0`) | `70D310A4` | 74,672 | 20,856 | 10,917 | Connected |
| `WormPostHardeningZeroed_20260914` (`-wormstrength=0`) | `70D310A4` | 74,672 | 20,856 | 10,917 | Connected |

Both runs also reported `canonical_repeat_equal=true`. The disabled asset path therefore matches
the hand-zeroed control byte-for-byte at the exported geometry hash, while exceeding the required
20,830 / 10,909 capability gate.

### Historical Part 2 — measured cheap field (reverted 2026-09-14)

The historical build selected `voxel.WormNoiseMode=1`: a deterministic scalar value-noise
evaluation using the same worm coordinate mapping, threshold, N1 short-circuit, N2 offset, and
strength contract. That selection was reverted after the ablation review. The current source is
exact-only Perlin mode 0; `voxel.WormNoiseMode` and `voxel.WormLatticeStep`, their storage, and
their switches no longer exist. The run names and measurements below are retained as historical
evidence only.

The coarser lattice was measured rather than assumed. The first lattice implementation was much
slower because of cache churn; after the fixed tile-window cache, it returned to roughly the
exact cost but did not improve it. Representative static worker seconds were exact 33.10,
lattice-2 initial 160.86, lattice-2 cache-fixed 33.78, lattice-4 cache-fixed 33.25, and exact
block-skip 33.10. The lattice path was a bounded historical experiment and is not retained. The
exact block skip is retained as a fail-closed exact-mode diagnostic;
it is not used as a correctness or performance claim for value noise because its Perlin proof
does not cover the changed field.

The capability and geometry candidates were:

| candidate | mode | geometry hash | triangles | player-fit / reach | result |
|---|---|---:|---:|---:|---|
| `WormExactCurrentExplore_20260914` | exact, lattice 0 | `311CA42C` | 75,482 | 20,830 / 10,909 | accepted control |
| `WormLedgerLattice2_20260914` | exact, lattice 2 | `5C9E0858` | 74,932 | 20,856 / 10,917 | capability pass; no speed gain |
| `WormLedgerLattice4_20260914` | exact, lattice 4 | `E3EC3FAF` | 74,708 | 20,857 / 10,917 | capability pass; no speed gain |
| `WormCheapValue_20260914` | value, lattice 0 | — | — | 20,826 / 10,917 | rejected: fit short by 4 |
| `WormCheapValueLattice2Explore_20260914` | value, lattice 2 | `C47567A2` | 75,132 | 20,857 / 10,917 | pass; no speed gain |
| `WormCheapFastHashExplore_20260914` | value, lattice 0 | `6204C7E6` | 75,956 | 20,851 / 10,917 | selected |

The historical selected mode changed the field, but the measured full-game gain was small rather than a
claim of a dramatic speedup. In two interleaved static pairs, exact averaged 0.087128 / 0.148415
s request-to-ready p50/p95, 0.063042 / 0.105080 s generation p50/p95, and 33.213871 worker s;
cheap averaged 0.087761 / 0.145897 s, 0.062863 / 0.106258 s, and 33.083774 worker s. In two
interleaved moving pairs at 800 cm/s, exact averaged 0.060116 / 0.131752 s request-to-ready,
0.050910 / 0.104262 s generation, and 109.165600 worker s; cheap averaged 0.061533 /
0.133272 s, 0.052125 / 0.105495 s, and 111.492032 worker s. Thus cheap is a small static worker
improvement and within noise there, but approximately 2.1% worse in moving worker seconds. That
is the measured tradeoff for the field change, not an overclaim.

The explicit game-path switch-off/on measurement requested for Part 2 used the force-off override,
with value-noise mode and no sampler. Static on averaged 0.085736 / 0.143135 s request-to-ready
p50/p95, 0.062793 / 0.104333 s generation p50/p95, and 33.022549 worker s. Static off averaged
0.084559 / 0.136829 s, 0.061727 / 0.099843 s, and 32.218070 worker s. Moving on averaged
0.059974 / 0.132163 s request-to-ready, 0.051983 / 0.102972 s generation, and 109.244366
worker s; moving off averaged 0.058397 / 0.126581 s, 0.050210 / 0.098518 s, and 105.648779
worker s. These are two interleaved A/B runs per side, 343 static and 1,521 moving LOD0 samples
per run, and all four had zero obsolete-tile aborts. Enabled worms therefore cost about 2.5%
static and 3.4% moving worker time in this harness, while the selected cheap field's exact-versus-
cheap comparison above measures the cost of changing the enabled field itself.

The final deterministic tile ledger used 64 tiles in `z_then_y_then_x` order. Exact A/B and cheap
A/B each had zero per-tile triangle-count/hash differences. Exact versus selected cheap changed
29/64 tile hashes/counts (45.3125%); the changed tiles contained 69,625/75,482 exact triangles
(92.2405%) and 70,099/75,956 cheap triangles (92.2890%), with a signed total of +474 triangles
and an absolute per-tile triangle-count delta of 1,822. The triangle percentages are explicitly
changed-tile coverage, not an assertion that individual triangles were matched across changed
topologies.

The four final deterministic/acceptance exports were:

| run pair | per-tile differences | hash / triangles | player-fit / reach | connectivity |
|---|---:|---|---:|---|
| `WormFinalExactDetA_20260914` / `WormFinalExactDetB_20260914` | 0 / 64 | `311CA42C` / 75,482 | 20,830 / 10,909 | Connected |
| `WormFinalCheapDetA_20260914` / `WormFinalCheapDetB_20260914` | 0 / 64 | `6204C7E6` / 75,956 | 20,851 / 10,917 | Connected |

The final fixed-camera LOD0 renders are 512×288 at
`-89.4815216,-104.9037323,-163.272038`:

- exact before: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\WormPostHardeningRenderBefore_20260914Out\render_02.png`;
- cheap after: `E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\WormPostHardeningRenderAfter_20260914Out\render_02.png`.

They show the same worm-rich passage with only small local wall/relief changes, consistent with
the tile ledger. The preliminary final static pair
`WormFinalPerfStaticOffA_20260914` / `WormFinalPerfStaticOnA_20260914` also completed, but both
were accidentally launched with mode 1 and are excluded from the exact-versus-cheap averages.

### Fresh moving sampler LOD0 top 20

The final post-hardening moving cheap run had 1,521 LOD0 samples, no obsolete tile aborts, and
the following offline-resolved exclusive leaf table. Percentages use the 71,427 LOD0 samples in
the offline reservoir:

| # | samples | percent | function |
|---:|---:|---:|---|
| 1 | 13,250 | 18.6704 | `GetDensityWithParams` |
| 2 | 3,632 | 5.1178 | `GetDensityAt` |
| 3 | 3,351 | 4.7218 | unknown |
| 4 | 2,750 | 3.8750 | `Sqrt` |
| 5 | 2,266 | 3.1930 | `SmoothMin` |
| 6 | 2,066 | 2.9112 | `FChunkSDFSpatialIndex::GetRange` |
| 7 | 1,708 | 2.4067 | `VF_ProjectNativePassageFloorUncached` |
| 8 | 1,678 | 2.3644 | `VF_ProjectTunnelSegmentXY` |
| 9 | 1,540 | 2.1700 | `VF_EvaluateSweptTunnelChain` |
| 10 | 1,360 | 1.9164 | `EvaluateTunnelCoreWorld` lambda |
| 11 | 1,267 | 1.7853 | `ProjectWalkableTunnelFloor` |
| 12 | 1,120 | 1.5782 | `EvaluateSDFCached` lambda |
| 13 | 1,116 | 1.5725 | swept-chain lambda |
| 14 | 1,035 | 1.4584 | `IsFiniteFast` |
| 15 | 875 | 1.2330 | `IsFinite` |
| 16 | 874 | 1.2315 | `GenerateMesh` |
| 17 | 842 | 1.1865 | `EvaluateModifierSDF` |
| 18 | 832 | 1.1724 | `DistanceSquaredToAabb` |
| 19 | 819 | 1.1540 | `FScopedTimer::End` |
| 20 | 789 | 1.1118 | `UsesOperatorStackForChunk` |

### Complete current-round launch ledger

The existing block-skip ledger above remains the record for the earlier `WormSkip*` launches.
This table adds the harness validation and every launch made for this off-switch/cheap-field
round, including invalidated, inconclusive, failed, and retried launches. A `PASS` here means the
process exited cleanly and produced its stated artifact; a candidate can still be rejected by a
capability or comparison gate.

| launch | outcome |
|---|---|
| `HarnessFixBuild_20260914` | PASS; harness/build validation completed. |
| `HarnessFixSamplerSmoke_20260914` | PASS; sampler-on static smoke, clean exit. |
| `HarnessFixSamplerSmokeRetry_20260914` | PASS; sampler-on static retry, clean exit. |
| `HarnessFixSamplerSmokeFinal_20260914` | PASS; sampler-on static final smoke, clean exit. |
| `HarnessFix01SamplerOnStatic.log` | PASS; sampler-on static. |
| `HarnessFix02SamplerOnMoving.log` | PASS; sampler-on moving, 64 m movement. |
| `HarnessFix03SamplerOnStatic.log` | PASS; sampler-on static. |
| `HarnessFix04SamplerOnMoving.log` | PASS; sampler-on moving, 64 m movement. |
| `HarnessFix05SamplerOnStatic.log` | PASS; sampler-on static. |
| `HarnessFix06SamplerOffMoving.log` | PASS; sampler-off moving; one obsolete-tile abort was recorded by the stress session. |
| `HarnessFix07SamplerOffStatic.log` | PASS; sampler-off static. |
| `HarnessFix08SamplerOnMoving.log` | PASS; sampler-on moving. |
| `HarnessFix09SamplerOffMoving.log` | PASS; sampler-off moving; nine obsolete-tile aborts were recorded by the stress session. |
| `HarnessFix10SamplerOnStatic.log` | PASS; sampler-on static. |
| `HarnessFixExport_20260914` | PASS; export commandlet exited 0. |
| `WormOnExact_20260914` | PASS; exact export-only baseline, hash `07C14005`, 153,346 triangles. |
| `WormOffSwitch_20260914` | PASS; disabled switch, hash `70D310A4`, 74,672 triangles, 20,856 / 10,917, Connected. |
| `WormZeroedControl_20260914` | PASS; hand-zeroed control, same hash `70D310A4`, 74,672 triangles. |
| `WormOfflineSamplerOn_20260914` | PASS; fresh exact sampler-on game run. |
| `OfflineSymbolizeWormSampler_20260914` | PASS; post-exit exact CSV resolved; 733 modules, zero identity mismatches. |
| `WormLedgerExact_20260914` | PASS; exact ledger candidate, 20,830 / 10,909, Connected. |
| `WormLedgerLattice2_20260914` | PASS; exact lattice-2 candidate. |
| `WormLedgerLattice4_20260914` | PASS; exact lattice-4 candidate. |
| `WormLatticeExactStaticA_20260914` | PASS; 343 LOD0 samples, worker 33.102516 s. |
| `WormLattice2StaticA_20260914` | PASS process; invalidated as a candidate after 160.860434 worker s and p95 generation 1.036767 s. |
| `WormLattice2StaticCacheFixA_20260914` | PASS process; invalidated partial cache experiment, 189 samples and 209.142420 worker s. |
| `WormLattice2StaticCacheFixB_20260914` | PASS; cache-fixed lattice-2 rerun, worker 33.779886 s. |
| `WormLattice4StaticCacheFixA_20260914` | PASS; cache-fixed lattice-4, worker 33.252895 s. |
| `WormLattice2StaticN2CacheA_20260914` | PASS; N2-cache experiment, worker 33.226479 s. |
| `WormBlockSkipStaticProbe_20260914` | PASS; exact block-skip probe, worker 33.099186 s. |
| `WormLattice2StaticN1CacheA_20260914` | PASS; N1-cache experiment, worker 33.618430 s. |
| `WormLedgerN1Exact_20260914` | PASS; exact N1 ledger candidate. |
| `WormLedgerN1Lattice2_20260914` | PASS; lattice-2 N1 ledger candidate. |
| `WormCheapValue_20260914` | PASS process; candidate rejected because player-fit was 20,826. |
| `WormCheapValueStaticA_20260914` | PASS; value-noise static profile, worker 32.551710 s. |
| `WormCheapValueLattice2StaticA_20260914` | PASS; value-noise lattice-2 profile, worker 33.814655 s. |
| `WormCheapValueLattice2Explore_20260914` | PASS; capability 20,857 / 10,917, Connected. |
| `WormCheapValue098Explore_20260914` | PASS process; candidate fit 20,821, rejected. |
| `WormCheapValue102Explore_20260914` | PASS process; candidate fit 20,827, rejected. |
| `WormCheapValue110Explore_20260914` | PASS process; candidate fit 20,828, rejected. |
| `WormCheapFastHashExplore_20260914` | PASS; selected cheap field, hash `6204C7E6`, 75,956 triangles, 20,851 / 10,917, Connected. |
| `WormExactCurrentExplore_20260914` | PASS; selected exact control, hash `311CA42C`, 75,482 triangles, 20,830 / 10,909, Connected. |
| `WormPerfStaticExactA_20260914` | PASS; preliminary exact static A. |
| `WormPerfStaticCheapA_20260914` | PASS; preliminary cheap static A. |
| `WormPerfStaticExactB_20260914` | PASS; preliminary exact static B. |
| `WormPerfStaticCheapB_20260914` | PASS; preliminary cheap static B. |
| `WormSamplerPlant_20260914` | PASS process; first planted profile, proof inconclusive. |
| `WormSamplerPlantOffline_20260914` | PASS process; first offline planted pass, no named plant row. |
| `WormSamplerPlantLong_20260914` | PASS process; longer planted profile, proof inconclusive. |
| `WormSamplerPlantLongOffline_20260914` | PASS process; longer offline pass, no named plant row. |
| `WormSamplerPlantGenerator_20260914` | PASS; generator-level planted profile. |
| `WormSamplerPlantGeneratorOffline_20260914` | PASS; five named plant rows resolved; proof accepted before plant removal. |
| `WormFinalOffSampler_20260914` | PASS; disabled sampler-on commandlet, zero worm rows after symbolization. |
| `WormFinalOffSymbolize_20260914` | PASS; post-exit disabled CSV resolved. |
| `WormFinalOffSwitch_20260914` | PASS; disabled geometry/capability proof. |
| `WormFinalZeroedControl_20260914` | PASS; zeroed-parameter control, exact geometry match. |
| `WormFinalExactDetA_20260914` | PASS; exact determinism A. |
| `WormFinalExactDetB_20260914` | PASS; exact determinism B. |
| `WormFinalCheapDetA_20260914` | PASS; cheap determinism A. |
| `WormFinalCheapDetB_20260914` | PASS; cheap determinism B. |
| `WormFinalPerfStaticOffA_20260914` | PASS process; preliminary pair member, but launched with cheap mode. |
| `WormFinalPerfStaticOnA_20260914` | PASS process; preliminary pair member, also cheap mode, excluded from comparison. |
| `WormFinalPerfStaticWormsOnA_20260914` | PASS; explicit game-path worms-on static A (`WormsForceOff=0`). |
| `WormFinalPerfStaticWormsOffA_20260914` | PASS; explicit game-path worms-off static A (`WormsForceOff=1`). |
| `WormFinalPerfStaticWormsOnB_20260914` | PASS; explicit game-path worms-on static B (`WormsForceOff=0`). |
| `WormFinalPerfStaticWormsOffB_20260914` | PASS; explicit game-path worms-off static B (`WormsForceOff=1`). |
| `WormFinalPerfStaticExactA_20260914` | PASS; final exact static A. |
| `WormFinalPerfStaticCheapA_20260914` | PASS; final cheap static A. |
| `WormFinalPerfStaticExactB_20260914` | PASS; final exact static B. |
| `WormFinalPerfStaticCheapB_20260914` | PASS; final cheap static B. |
| initial `WormFinalPerfMovingExactA_20260914` wrapper launch | FAILED harness shutdown after the graceful-close timeout; no crash folder or reporter, and the preserved backup log is `WormFinalPerfMovingExactA_20260914-backup-2026.09.14-05.14.32.log`. |
| retried `WormFinalPerfMovingExactA_20260914` | PASS; rerun used the runtime movement/exit path, 1,521 LOD0 samples. |
| `WormFinalPerfMovingCheapA_20260914` | PASS; final cheap moving A. |
| `WormFinalPerfMovingExactB_20260914` | PASS; final exact moving B. |
| `WormFinalPerfMovingCheapB_20260914` | PASS; final cheap moving B. |
| `WormFinalPerfMovingWormsOnA_20260914` | PASS; explicit game-path worms-on moving A (`WormsForceOff=0`). |
| `WormFinalPerfMovingWormsOffA_20260914` | PASS; explicit game-path worms-off moving A (`WormsForceOff=1`). |
| `WormFinalPerfMovingWormsOnB_20260914` | PASS; explicit game-path worms-on moving B (`WormsForceOff=0`). |
| `WormFinalPerfMovingWormsOffB_20260914` | PASS; explicit game-path worms-off moving B (`WormsForceOff=1`). |
| `WormFinalMovingSamplerOn_20260914` | PASS; pre-hardening cheap sampler-on moving profile. |
| `WormFinalMovingSamplerOffline_20260914` | PASS; pre-hardening post-exit symbolization. |
| `WormFinalRenderBefore_20260914` | PASS; exact fixed-camera render. |
| `WormFinalRenderAfter_20260914` | PASS; cheap fixed-camera render. |
| `WormPostHardeningOff_20260914` | PASS; final disabled geometry proof. |
| `WormPostHardeningZeroed_20260914` | PASS; final zeroed-control geometry proof. |
| `WormFinalMovingSamplerOnPostHardening_20260914` | PASS; final cheap sampler-on moving profile, 1,521 LOD0 samples, 800 cm/s, no obsolete aborts. |
| `WormFinalMovingSamplerOfflinePostHardening_20260914` | PASS; final cheap offline symbolization, zero module identity mismatches. |
| `WormFinalMovingSamplerExactProof_20260914` | PASS; final exact sampler-on comparison profile. |
| `WormFinalMovingSamplerExactProofOffline_20260914` | PASS; final exact offline symbolization, zero module identity mismatches. |
| `WormPostHardeningRenderBefore_20260914` | PASS; final exact render refresh. |
| `WormPostHardeningRenderAfter_20260914` | PASS; final cheap render refresh. |
| `PerfSampledBuild_FinalForceOff.log` | PASS; final force-off wiring build, Result: Succeeded. |
| `WormFinalForceOffBuildSamplerOnMoving_20260914` | PASS; final sampler-on moving run on the force-off build, 1,521 LOD0 samples, no obsolete aborts. |
| `WormFinalForceOffBuildSamplerOffline_20260914` | FAILED; first direct post-exit symbolizer attempt stopped on UE 5.7's fatal no-writable-cache-graph DDC configuration; preserved log/output, no code failure. |
| `WormFinalForceOffBuildSamplerOfflineRetry_20260914` | PASS; retried with writable `Saved\ZenData`/`Saved\DDC`, 733 modules, 10 loaded, zero identity mismatches, all 7,795 unique PCs mapped. |
| `PerfSampledBuild_FinalFingerprint.log` | PASS; final cache-key hardening rebuild, Result: Succeeded. |
| `WormFinalFingerprintSmoke_20260914` | PASS; post-hardening clean game smoke, exact staged runtime verified, writable DDC, zero crash folders/reporters. |
## Cheap-worm revert and TunnelNetwork ablation round — 2026-09-14

This section supersedes the historical cheap-worm selection above. The earlier value-noise
experiment changed the field without producing a gain: static movement was within noise and the
moving worker result was about 2.1% slower. The production/default path therefore keeps exact
Perlin.

### Revert and exactness

- voxel.WormNoiseMode and voxel.WormLatticeStep, their storage, and their switches are removed.
- VoxelWormField now has one evaluation path: exact Perlin, with the existing N1 short circuit.
- The exact block skip remains available as voxel.WormBlockSkip, default 0. The asset-owned
  bEnableWorms and the measurement-only voxel.WormsForceOff remain.
- The generation fingerprint still hashes bEnableWorms and WormsForceOff, and now also hashes the
  resolved TunnelNetwork ablation mask.
- The new ablation controls are development-only, resolve once per process, are world-changing,
  and are never gameplay or multiplayer switches. A nonzero bit means force that stage off; zero
  means all stages on. Shipping builds resolve the mask to zero.

The prescribed UBT command from this file was used after synchronizing the staged host copy under
the plugin Saved directory. The final build log is
E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\PerfSampledBuild_Ablation.log and reports
Result: Succeeded. The loaded runtime was
E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Binaries\Win64\UnrealEditor-VoxelForge.dll
with SHA-256 A70528AC82641D7D610D8BA9497D0322B1EBDE22978B1E826A97FC4C0D038149. The editor module
SHA-256 was 8EA2A71BB29F828C5DE7559EC200AC4E86CF61F7DB5C8470406208073E22456D.

The two default commandlets both used
E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\DDC and
E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\ZenData:

- AblationDefaultExact_20260914Out: mask 0; status ok; canonical 128^3 export SHA-256
  B3E5F4C398DD0547C6C55F415872058246D4A292C2F0D636EE1CD0829DE9B377; geometry CRC 07C14005;
  82,273 vertices and 153,346 triangles.
- AblationDefaultCapability_20260914Out: mask 0; geometry CRC 311CA42C; 75,482 triangles;
  player_fit_volume_cells 20,830; reachable_player_fit_cells 10,909; walk-only reachable
  10,909; Connected; deterministic JSON.

The current game startup trace
E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\AblationDefaultStaticTrace_20260914.startup.jsonl
was compared by tile key to
E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\NextHeadStaticTrace_20260913.startup.jsonl:
841 versus 841 tile records, zero missing, zero extra, zero verdict/empty/triangle mismatches,
and identical all-tile and LOD0 triangle totals of 669,834 and 325,204. The current trace
finished with desired_set_satisfied_and_queue_drained and the loaded runtime hash above.

### Ablation controls

| bit | CVar | stage forced off |
|---:|---|---|
| 0x0001 | voxel.TunnelAblateCaveWarp | the three cave-warp Perlin calls; CaveWarpStrength resolves to zero |
| 0x0002 | voxel.TunnelAblateDetailOps | detail noise, roughness, and terrain operations |
| 0x0004 | voxel.TunnelAblateRoomSDF | room SDF and room-SDF joins in EvaluateSDFCached |
| 0x0008 | voxel.TunnelAblateTunnelSDF | tunnel SDF in EvaluateSDFCached |
| 0x0010 | voxel.TunnelAblateTunnelCore | EvaluateTunnelCoreWorld, support/core SDF work, and its GetDensityAt tail |
| 0x0020 | voxel.TunnelAblatePassageCarving | EvaluateModifierSDF and passage carving |
| 0x0040 | voxel.TunnelAblatePassageStructuralPosts | passage structural posts and legacy structural floor support |
| 0x0080 | voxel.TunnelAblateNativeFloor | native floor composition |
| 0x0100 | voxel.TunnelAblateDisturbances | ApplyDisturbances |
| 0x0200 | voxel.TunnelAblateOriginSpine | origin spine |
| 0x0400 | voxel.TunnelAblateBoundarySeal | boundary seal |
| 0x0800 | voxel.TunnelAblateLandingPosts | landing posts and landing-floor posts |
| 0x1000 | voxel.TunnelAblateXYEdgeSeal | XY edge seal |
| 0x2000 | voxel.TunnelAblatePitChimneySDF | pit and chimney SDF work |

Each override is applied in the fused evaluator, operator-stack source/classifier paths, cached
SDF/core paths, and modifier/floor paths where that stage exists. The resolved mask is emitted in
the explore JSON and is part of the generation fingerprint. This is an ablation instrument, not a
candidate field.

### Measurement method and noise

All game runs were sampler-off, NullRHI, operator block on, fused evaluator on, outer classifier
off, and used a 15-second clean session. Static runs produced 343 LOD0 samples and 841 applied
tiles. Each static row has two off runs; the two all-on static baselines are shared and interleaved
across the two passes. The all-on static mean was 33.064458 worker seconds, generation p50
0.064657 s, generation p95 0.105786 s. Baseline worker range was 0.27%; baseline generation-p95
range was 1.62%.

The table reports the mean of the two off runs, the saving against that all-on mean, and the
off-run worker range as a compact noise indicator. Positive saving means the off run used fewer
worker seconds. All rows completed 841 tiles with zero obsolete aborts.

| component removed | runs | off worker s | saving s (%) | off generation p50 s | off generation p95 s | off worker range |
|---|---:|---:|---:|---:|---:|---:|
| tunnel SDF | 2 | 29.304 | 3.760 (11.37%) | 0.060813 | 0.084811 | 0.53% |
| cave warp | 2 | 29.654 | 3.411 (10.31%) | 0.056881 | 0.098260 | 2.03% |
| passage carving | 2 | 29.730 | 3.334 (10.08%) | 0.056336 | 0.096892 | 0.88% |
| landing posts | 2 | 29.800 | 3.264 (9.87%) | 0.056776 | 0.097884 | 1.33% |
| passage structural posts | 2 | 31.205 | 1.859 (5.62%) | 0.060143 | 0.099723 | 0.50% |
| worms | 2 | 32.265 | 0.799 (2.42%) | 0.064232 | 0.099511 | 0.83% |
| native floor | 2 | 32.347 | 0.718 (2.17%) | 0.063590 | 0.103897 | 0.45% |
| origin spine | 2 | 32.529 | 0.535 (1.62%) | 0.063174 | 0.102446 | 0.61% |
| room SDF | 2 | 32.542 | 0.522 (1.58%) | 0.064194 | 0.100999 | 0.90% |
| pit/chimney SDF | 2 | 32.689 | 0.375 (1.13%) | 0.064599 | 0.103472 | 0.26% |
| boundary seal | 2 | 32.814 | 0.251 (0.76%) | 0.064863 | 0.104105 | 1.21% |
| detail ops | 2 | 32.838 | 0.227 (0.69%) | 0.065330 | 0.100864 | 0.69% |
| XY edge seal | 2 | 32.901 | 0.164 (0.49%) | 0.065021 | 0.104411 | 1.30% |
| disturbances | 2 | 33.208 | -0.144 (-0.43%) | 0.065100 | 0.106342 | 0.53% |
| tunnel core | 2 | 95.468 | -62.404 (-188.73%) | 0.060597 | 0.098965 | 0.07% |

The field was intentionally allowed to change. For example, tunnel-SDF off produced 643,388
triangles, cave-warp off 655,726, passage-carving off 654,932, and tunnel-core off 655,654,
against the all-on 669,834 static triangles. Some low-cost rows retained the same mesh totals in
this window; they are still world-changing overrides and are not correctness candidates.

The sum of positive standalone static savings is 19.218608 worker seconds, 58.125% of the
33.064458-second baseline, leaving 13.845850 seconds (41.875%) as shared/unattributed cost in
this ablation accounting. The signed sum over every row is -43.329105 seconds because tunnel-core
removal is a large regression caused by the changed field; the rows are not additive and this is
why the signed sum is not a speedup prediction.

Moving confirmation covered the five largest positive static savers. Each row has two fresh
all-on baselines interleaved with two off runs. Movement began at 3 seconds and ran at 800 cm/s;
each run produced 882 LOD0 samples and 1,680 applied tiles, with zero obsolete aborts.

| component removed | runs | all-on worker s | off worker s | saving s (%) | all-on gen p50/p95 s | off gen p50/p95 s | baseline/off worker range |
|---|---:|---:|---:|---:|---:|---:|---:|
| tunnel SDF | 2+2 | 72.218 | 63.847 | 8.371 (11.59%) | 0.063238 / 0.104331 | 0.060117 / 0.082540 | 0.71% / 0.87% |
| cave warp | 2+2 | 72.810 | 65.010 | 7.800 (10.71%) | 0.064610 / 0.105986 | 0.055398 / 0.094423 | 0.46% / 1.14% |
| landing posts | 2+2 | 72.556 | 64.763 | 7.794 (10.74%) | 0.063820 / 0.105220 | 0.055539 / 0.096228 | 1.13% / 2.36% |
| passage carving | 2+2 | 72.043 | 64.552 | 7.492 (10.40%) | 0.063507 / 0.104761 | 0.055276 / 0.096373 | 1.07% / 1.72% |
| passage structural posts | 2+2 | 72.417 | 68.093 | 4.324 (5.97%) | 0.063779 / 0.104611 | 0.059068 / 0.101321 | 0.14% / 0.83% |

The moving standalone saving sum is 35.780708 worker seconds. Because these are five separate
ablations with separate baseline pairs, it is not a combined-speedup forecast; against the mean
72.409-second baseline per row it is 49.4%, leaving 36.628 seconds of shared/unattributed work.

### Ranking and plausible cheaper forms

1. Tunnel SDF — the largest measured real cost in both paths. A plausible exact optimization is
   sharing one cached swept-tunnel result between EvaluateSDFCached, tunnel-core metadata, and
   the final density tail, while preserving the existing evaluation order and float values. That
   would be field-preserving. A lower-resolution or analytic approximation would be field-changing
   and would need a fresh capability/export gate.
2. Cave warp — the three Perlin calls are expensive in the ablation. Exact reuse of the same
   coordinates/results across the evaluator, cached graph, and op-stack paths is plausible and
   field-preserving. Reducing frequency or replacing the noise is field-changing.
3. Passage carving — exact memoization/reuse of modifier SDF and passage candidate results across
   EvaluateModifierSDF, ApplyPassageCarvingOnly, and the structural/floor queries is plausible and
   field-preserving. Simplifying the carving geometry is field-changing.
4. Landing posts — exact per-tile reuse of landing/post/floor metadata is plausible and
   field-preserving. Removing or thinning landing posts is field-changing.
5. Passage structural posts — exact reuse of structural-post SDF/metadata is plausible and
   field-preserving. Removing or merging posts is field-changing.

Worms is sixth at 2.42% static and is now kept exact because the cheap alternatives had no gain.
Native floor and origin spine follow at 2.17% and 1.62%. Room SDF, detail ops, seals,
disturbances, and pit/chimney work are not attractive next-round targets on these measurements.
Tunnel-core removal is explicitly not a candidate: it increased worker time by about 2.9x.
These ablations explain why the old 19.4% line-level worm figure and the proposed tunnel-core
line targets were unsafe attribution guides.

### Complete launch ledger for this round

All game and commandlet logs below are under
E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved. Every commandlet used the writable DDC path
E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\DDC. No offline symbolizer was needed or
launched in this round. Every Unreal process that ran exited 0, verified the loaded runtime
SHA-256 A70528AC82641D7D610D8BA9497D0322B1EBDE22978B1E826A97FC4C0D038149, and had no crash or
retry; the two parser-only attempts below launched no Unreal process. Peak observed private
memory was approximately 1.54–1.60 GiB per process, with no runaway growth.

| launch | outcome |
|---|---|
| initial prescribed staged build | UP-TO-DATE report; staged host was stale, so no measurement was taken |
| PerfSampledBuild_Ablation | PASS; prescribed UBT build, Result: Succeeded |
| AblationDefaultExact_20260914 | PASS; canonical export exact |
| AblationDefaultCapability_20260914 | PASS; 311CA42C, 75,482 triangles, 20,830 / 10,909 |
| static ablation batch launch 1 | FAILED before process start; PowerShell parser error from a missing DDC-argument quote |
| static ablation batch launch 2 | FAILED before process start; same quoting error; no Unreal process |
| AblationStaticBaseA_20260914 | PASS |
| AblationStaticBaseB_20260914 | PASS |
| AblationStaticCaveWarpA_20260914 | PASS |
| AblationStaticCaveWarpB_20260914 | PASS |
| AblationStaticDetailOpsA_20260914 | PASS |
| AblationStaticDetailOpsB_20260914 | PASS |
| AblationStaticRoomSDFA_20260914 | PASS |
| AblationStaticRoomSDFB_20260914 | PASS |
| AblationStaticTunnelSDFA_20260914 | PASS |
| AblationStaticTunnelSDFB_20260914 | PASS |
| AblationStaticTunnelCoreA_20260914 | PASS; 95.436091 worker seconds |
| AblationStaticTunnelCoreB_20260914 | PASS; 95.500636 worker seconds |
| AblationStaticPassageCarvingA_20260914 | PASS |
| AblationStaticPassageCarvingB_20260914 | PASS |
| AblationStaticPassageStructuralPostsA_20260914 | PASS |
| AblationStaticPassageStructuralPostsB_20260914 | PASS |
| AblationStaticNativeFloorA_20260914 | PASS |
| AblationStaticNativeFloorB_20260914 | PASS |
| AblationStaticDisturbancesA_20260914 | PASS |
| AblationStaticDisturbancesB_20260914 | PASS |
| AblationStaticOriginSpineA_20260914 | PASS |
| AblationStaticOriginSpineB_20260914 | PASS |
| AblationStaticBoundarySealA_20260914 | PASS |
| AblationStaticBoundarySealB_20260914 | PASS |
| AblationStaticLandingPostsA_20260914 | PASS |
| AblationStaticLandingPostsB_20260914 | PASS |
| AblationStaticXYEdgeSealA_20260914 | PASS |
| AblationStaticXYEdgeSealB_20260914 | PASS |
| AblationStaticPitChimneySDFA_20260914 | PASS |
| AblationStaticPitChimneySDFB_20260914 | PASS |
| AblationStaticWormsOffA_20260914 | PASS |
| AblationStaticWormsOffB_20260914 | PASS |
| AblationMovingTunnelSDFBaseA_20260914 | PASS |
| AblationMovingTunnelSDFA_20260914 | PASS |
| AblationMovingTunnelSDFBaseB_20260914 | PASS |
| AblationMovingTunnelSDFB_20260914 | PASS |
| AblationMovingCaveWarpBaseA_20260914 | PASS |
| AblationMovingCaveWarpA_20260914 | PASS |
| AblationMovingCaveWarpBaseB_20260914 | PASS |
| AblationMovingCaveWarpB_20260914 | PASS |
| AblationMovingPassageCarvingBaseA_20260914 | PASS |
| AblationMovingPassageCarvingA_20260914 | PASS |
| AblationMovingPassageCarvingBaseB_20260914 | PASS |
| AblationMovingPassageCarvingB_20260914 | PASS |
| AblationMovingLandingPostsBaseA_20260914 | PASS |
| AblationMovingLandingPostsA_20260914 | PASS |
| AblationMovingLandingPostsBaseB_20260914 | PASS |
| AblationMovingLandingPostsB_20260914 | PASS |
| AblationMovingPassageStructuralPostsBaseA_20260914 | PASS |
| AblationMovingPassageStructuralPostsA_20260914 | PASS |
| AblationMovingPassageStructuralPostsBaseB_20260914 | PASS |
| AblationMovingPassageStructuralPostsB_20260914 | PASS |
| AblationDefaultStaticTrace_20260914 | PASS; current 841-tile trace, exact keyed comparison |

No .uasset was edited. Engine source and binaries were not modified. No commit, push, or stash was
performed.

## Current-tip cost re-rank — 2026-09-15 (1d824aa)

This is a measurement-only round on the committed experimental tip. Generation source,
configuration assets, and .uasset files were not edited. The ignored staged host outputs under
E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost were rebuilt as part of the measurement;
no commit, push, or stash was performed.

### Build and loaded runtime

The owner/editor check was clear before the build and before every Unreal launch. The prescribed
build completed successfully:

    dotnet "E:\Program Files\Epic Games\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll" UnrealEditor Win64 Development "-Project=E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\HostProject.uproject" -WaitMutex -FromMsBuild -architecture=x64 -NoUBA "-Log=E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\PerfSampledBuild_ReRank_20260915.log"
    Result: Succeeded; Target is up to date; total execution time 1.31 s.

The runtime loaded by every game and commandlet process was
E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Binaries\Win64\UnrealEditor-VoxelForge.dll,
SHA-256 70F5F69DCCBCE28AA0C1AC89E92040EBDB78FBC58F1F5FE07BD958F3C38DF0B4. The editor module
built alongside it was SHA-256 C2081DB9B14D7E43A31A218200064FA941643E40DEAA9372E9067E72B6D966C6.
Each launched process observed the runtime module at that staged path and matched the runtime
hash. The build log is
E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\PerfSampledBuild_ReRank_20260915.log.

### Method and current static table

The game command line was the same as the 6ccf371 ablation method: staged HostProject, -nullrhi,
operator block on, fused evaluator on, outer classifier off, tile cache and spatial index on,
-voxel.TestExitSeconds=15, sampler off, writable
E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\DDC and Saved\ZenData, and an absolute
-abslog= path. Static sessions generated 343 LOD0 samples and 841 applied tiles with zero
obsolete aborts. The two all-on static baselines were shared by every row and were run before the
remove-one pairs: 29.368018 s and 29.229716 s worker time, mean 29.298867 s; baseline generation
p50/p95 was 0.053218 / 0.095652 s.

The table reports the mean of two off runs, the saving against that shared all-on mean, and the
changed-field triangle total. The percentage is the signed standalone worker saving share; rows
are not additive, and a field-changing ablation is not a production speedup claim.

| component removed | control | off worker s | saving s (%) | off generation p50/p95 s | triangles A/B |
|---|---|---:|---:|---:|---:|
| tunnel SDF | 0x0008 | 25.595623 | 3.703244 (12.64%) | 0.050373 / 0.073456 | 643424 / 643424 |
| cave warp | 0x0001 | 27.340968 | 1.957899 (6.68%) | 0.048299 / 0.092184 | 655734 / 655734 |
| passage carving | 0x0020 | 26.639516 | 2.659351 (9.08%) | 0.046557 / 0.090078 | 655000 / 655000 |
| landing posts | 0x0800 | 29.047863 | 0.251005 (0.86%) | 0.052757 / 0.095932 | 669902 / 669902 |
| passage structural posts | 0x0040 | 27.352201 | 1.946666 (6.64%) | 0.048393 / 0.089721 | 669902 / 669902 |
| worms | voxel.WormsForceOff=1 | 28.242978 | 1.055889 (3.60%) | 0.052127 / 0.091638 | 662342 / 662342 |
| native floor | 0x0080 | 28.302872 | 0.995995 (3.40%) | 0.051091 / 0.093253 | 671138 / 671138 |
| origin spine | 0x0200 | 28.857599 | 0.441268 (1.51%) | 0.051791 / 0.095496 | 670350 / 670350 |
| room SDF | 0x0004 | 28.055889 | 1.242978 (4.24%) | 0.050986 / 0.090057 | 535220 / 535220 |
| pit/chimney SDF | 0x2000 | 29.146807 | 0.152060 (0.52%) | 0.052071 / 0.098243 | 669902 / 669902 |
| boundary seal | 0x0400 | 29.447356 | -0.148489 (-0.51%) | 0.052806 / 0.094607 | 669902 / 669902 |
| detail ops | 0x0002 | 29.957673 | -0.658805 (-2.25%) | 0.054265 / 0.095532 | 666972 / 666972 |
| XY edge seal | 0x1000 | 29.071525 | 0.227342 (0.78%) | 0.052822 / 0.097379 | 669902 / 669902 |
| disturbances | 0x0100 | 28.832774 | 0.466093 (1.59%) | 0.052326 / 0.094430 | 669902 / 669902 |
| tunnel core | 0x0010 | 23.192249 | 6.106618 (20.84%) | 0.046526 / 0.068197 | 655666 / 655666 |

All 14 voxel.TunnelAblate* bits were run twice. The extra worms row is included so this table
remains row-comparable with the 6ccf371 table; worms is controlled by voxel.WormsForceOff, not by
a TunnelAblate* bit. The current all-on field is 669902 triangles in this window. Different
triangle counts show that these overrides are field-changing; equal triangle totals do not prove
field identity.

### Static share against 6ccf371

These are standalone remove-one saving shares, not inclusive profiler shares. Now uses the
29.298867 s current all-on mean above. The historical column is copied from the 6ccf371
remove-one table, whose all-on mean was 33.064458 s. Change is percentage points.

| component | share now | share at 6ccf371 | change |
|---|---:|---:|---:|
| tunnel SDF | 12.64% | 11.37% | +1.27 pp |
| cave warp | 6.68% | 10.31% | -3.63 pp |
| passage carving | 9.08% | 10.08% | -1.00 pp |
| landing posts | 0.86% | 9.87% | -9.01 pp |
| passage structural posts | 6.64% | 5.62% | +1.02 pp |
| worms | 3.60% | 2.42% | +1.18 pp |
| native floor | 3.40% | 2.17% | +1.23 pp |
| origin spine | 1.51% | 1.62% | -0.11 pp |
| room SDF | 4.24% | 1.58% | +2.66 pp |
| pit/chimney SDF | 0.52% | 1.13% | -0.61 pp |
| boundary seal | -0.51% | 0.76% | -1.27 pp |
| detail ops | -2.25% | 0.69% | -2.94 pp |
| XY edge seal | 0.78% | 0.49% | +0.29 pp |
| disturbances | 1.59% | -0.43% | +2.02 pp |
| tunnel core | 20.84% | -188.73% | +209.57 pp |

The old tunnel-core row was a changed-field regression and is not a valid old cost estimate. The
current tunnel-core saving is likewise a changed-field diagnostic, not permission to remove the
stage. Summing only positive current standalone savings gives 21.206406 worker seconds, or
72.3796% of the current static baseline. The remaining 27.6204%, 8.092461 s, is the current
unattributed/shared cost in this ablation accounting. The signed rows are not a combined speedup
forecast.

### Moving confirmation for the current top five

Moving sessions used the same switches, began at 3 s, ran at 800 cm/s, and produced 882 LOD0
samples and 1680 applied tiles per process with zero obsolete aborts. Each row has two fresh
all-on baselines interleaved with two remove-one runs. The worker-share column is against that
row's own all-on mean.

| component removed | all-on worker s | off worker s | saving s (%) | all-on gen p50/p95 s | off gen p50/p95 s | off triangles |
|---|---:|---:|---:|---:|---:|---:|
| tunnel core | 62.803210 | 50.841671 | 11.961539 (19.05%) | 0.051890 / 0.094224 | 0.045179 / 0.071081 | 1310998 / 1310998 |
| tunnel SDF | 62.793729 | 54.886260 | 7.907468 (12.59%) | 0.051844 / 0.095083 | 0.049000 / 0.074435 | 1274742 / 1274742 |
| passage carving | 63.103260 | 56.867475 | 6.235784 (9.88%) | 0.051921 / 0.094814 | 0.045053 / 0.089081 | 1315772 / 1315772 |
| cave warp | 63.121345 | 59.108887 | 4.012458 (6.36%) | 0.052307 / 0.094549 | 0.048029 / 0.091797 | 1354749 / 1354749 |
| passage structural posts | 63.000096 | 59.544697 | 3.455399 (5.48%) | 0.052444 / 0.095416 | 0.048040 / 0.090804 | 1331550 / 1331550 |

The moving table confirms the current top five. Cave warp is materially below its old 10.71%
moving share after the exact SIMD batching in 1d824aa; landing posts no longer belongs in the top
five after the near-free reach-gated structural path.

### Sampler-on moving hint and offline symbolization

One sampler-on moving run used -voxel.SampleStacks=1000, a 30 s session beginning at 5 s,
800 cm/s movement, and the same clean NullRHI command line. Its process result was 1521 LOD0
samples, 2807 applied tiles, 2315204 triangles, and 101.786811 worker seconds. The raw and
offline artifacts are:

- E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\VoxelStackSamples_game_21444.csv
- E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\VoxelStackModules_game_21444.tsv
- E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\BuildHost\HostProject\Plugins\VoxelForge\Saved\ReRankSamplerMovingOffline_20260915.txt
- E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\ReRankSamplerMovingOn_20260915.log
- E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Saved\ReRankSamplerMovingOffline_20260915.log

The offline summary has 95308 samples with stacks: 62031 LOD0 and 33277 LOD1+. It loaded
9/9 needed modules, has zero module-identity mismatches, maps 7335/7335 PCs, and has zero
untagged samples. The inclusive top 20s below are a hint only. The engine task wrappers dominate
the top ranks; component claims are taken from the ablations above, not inferred from one source
line.

#### All LOD inclusive top 20

| rank | samples | share | label | function | source |
|---:|---:|---:|---|---|---|
| 1 | 95306 | 99.9979% | engine | [Inline] Invoke() | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Templates\Invoke.h:47 |
| 2 | 95306 | 99.9979% | engine | [Inline] LowLevelTasks::FTask::Init lambda | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Async\Fundamental\Task.h:499 |
| 3 | 95306 | 99.9979% | engine | [Inline] TTaskDelegate::Call | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Async\Fundamental\TaskDelegate.h:162 |
| 4 | 95306 | 99.9979% | engine | [Inline] FTaskBase::Init lambda | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Tasks\TaskPrivate.h:180 |
| 5 | 95306 | 99.9979% | engine | TTaskDelegate::CallAndMove | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Async\Fundamental\TaskDelegate.h:171 |
| 6 | 95306 | 99.9979% | engine | FTaskBase::TryExecuteTask | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Tasks\TaskPrivate.h:518 |
| 7 | 95306 | 99.9979% | engine | TExecutableTaskBase::ExecuteTask | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Tasks\TaskPrivate.h:898 |
| 8 | 95306 | 99.9979% | plugin | AVoxelWorld::LoadTile lambda | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp:4752 |
| 9 | 95305 | 99.9969% | plugin | AVoxelWorld::GenerateTileResult() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp:5159 |
| 10 | 95094 | 99.7755% | plugin | UVoxelMarchingCubesMesher::GenerateMesh() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelMarchingCubesMesher.cpp:663 |
| 11 | 92346 | 96.8922% | plugin | UVoxelGenerator::GetDensityAt() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp:3263 |
| 12 | 58256 | 61.1239% | plugin | UVoxelGenerator::GetDensityWithParams() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp:4093 |
| 13 | 26070 | 27.3534% | plugin | VF_ForEachSpatialCandidate | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelCaveMorphology.cpp:1473 |
| 14 | 16321 | 17.1245% | plugin | VoxelCaveMorphology::EvaluateSDFCached() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelCaveMorphology.cpp:7847 |
| 15 | 12382 | 12.9916% | plugin | VF_EvaluateSweptTunnel() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelCaveMorphology.cpp:1070 |
| 16 | 12044 | 12.6369% | plugin | VoxelCaveMorphology::EvaluateTunnelCoreWorld() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelCaveMorphology.cpp:8032 |
| 17 | 11862 | 12.4460% | plugin | VF_EvaluateSweptTunnelChain() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelCaveMorphology.cpp:986 |
| 18 | 10781 | 11.3117% | plugin | EvaluateSDFCached lambda | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelCaveMorphology.cpp:7836 |
| 19 | 10562 | 11.0820% | plugin | UVoxelStrateManager::ApplyPassageCarvingOnly() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelStrateManager.cpp:2727 |
| 20 | 9904 | 10.3916% | plugin | EvaluateTunnelCoreWorld lambda | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelCaveMorphology.cpp:8025 |

#### LOD0 inclusive top 20

| rank | samples | share | label | function | source |
|---:|---:|---:|---|---|---|
| 1 | 62029 | 99.9968% | engine | [Inline] Invoke() | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Templates\Invoke.h:47 |
| 2 | 62029 | 99.9968% | engine | [Inline] FTask::Init lambda | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Async\Fundamental\Task.h:499 |
| 3 | 62029 | 99.9968% | engine | [Inline] TTaskDelegate::Call | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Async\Fundamental\TaskDelegate.h:162 |
| 4 | 62029 | 99.9968% | engine | [Inline] FTaskBase::Init lambda | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Tasks\TaskPrivate.h:180 |
| 5 | 62029 | 99.9968% | engine | TTaskDelegate::CallAndMove | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Async\Fundamental\TaskDelegate.h:171 |
| 6 | 62029 | 99.9968% | engine | FTaskBase::TryExecuteTask | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Tasks\TaskPrivate.h:518 |
| 7 | 62029 | 99.9968% | engine | TExecutableTaskBase::ExecuteTask | E:\Program Files\Epic Games\UE_5.7\Engine\Source\Runtime\Core\Public\Tasks\TaskPrivate.h:898 |
| 8 | 62029 | 99.9968% | plugin | AVoxelWorld::LoadTile lambda | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp:4752 |
| 9 | 62028 | 99.9952% | plugin | AVoxelWorld::GenerateTileResult() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelWorld.cpp:5159 |
| 10 | 61915 | 99.8130% | plugin | UVoxelMarchingCubesMesher::GenerateMesh() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelMarchingCubesMesher.cpp:663 |
| 11 | 60168 | 96.9967% | plugin | UVoxelGenerator::GetDensityAt() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp:3263 |
| 12 | 39193 | 63.1829% | plugin | UVoxelGenerator::GetDensityWithParams() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelGenerator.cpp:4093 |
| 13 | 17762 | 28.6341% | plugin | VF_ForEachSpatialCandidate | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelCaveMorphology.cpp:1473 |
| 14 | 11212 | 18.0748% | plugin | VoxelCaveMorphology::EvaluateSDFCached() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelCaveMorphology.cpp:7847 |
| 15 | 8730 | 14.0736% | plugin | VF_EvaluateSweptTunnel() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelCaveMorphology.cpp:1070 |
| 16 | 8379 | 13.5078% | plugin | VF_EvaluateSweptTunnelChain() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelCaveMorphology.cpp:986 |
| 17 | 8346 | 13.4546% | plugin | VoxelCaveMorphology::EvaluateTunnelCoreWorld() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelCaveMorphology.cpp:8032 |
| 18 | 7523 | 12.1278% | plugin | EvaluateSDFCached lambda | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelCaveMorphology.cpp:7836 |
| 19 | 7135 | 11.5023% | plugin | UVoxelStrateManager::ApplyPassageCarvingOnly() | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelStrateManager.cpp:2727 |
| 20 | 6937 | 11.1831% | plugin | EvaluateTunnelCoreWorld lambda | E:\Projet Unreal\VoxelM\Plugins\VoxelForge\Source\VoxelForge\Private\VoxelCaveMorphology.cpp:8025 |

The symbolizer is not an attribution instrument: inclusive rows overlap, and the sampler itself
adds overhead. It supports the same shape as the ablation result (SDF, swept tunnel, core, and
passage carving are visible), but any line-level statement must be confirmed by a remove-one row.

### Next three targets

1. **Warped tunnel SDF + world-space tunnel core merge — field-changing.** The current tunnel-core
   remove-one is the largest standalone result (20.84% static, 19.05% moving), and tunnel SDF is
   independently 12.64% / 12.59%. A plausible form is one warped tunnel/core representation that
   supplies the final density and core/floor metadata once. This changes the world-space core
   semantics, so it requires the capability/export gates (player-fit 20,830; walk-reachable
   10,909), render review, and determinism checks. The two tunnel rows are not additive and are
   not a 33% forecast.
2. **Passage carving reuse — exact.** The measured share is 9.08% static and 9.88% moving. A
   cheaper exact form is per-tile reuse of the existing passage candidate list and modifier-SDF
   result across EvaluateModifierSDF, carving, and floor/structural queries, preserving the
   current values and evaluation order. Any changed carving geometry or threshold is
   field-changing instead.
3. **Cave warp — exact.** The remaining share is 6.68% static and 6.36% moving after the exact
   three-channel SIMD batch in 1d824aa. The next exact form is reuse of the already computed warp
   coordinates/results across the evaluator, cached graph, and operator-stack paths. A frequency,
   hash, or approximation change is field-changing. Passage structural posts are the next
   measured moving target at 5.48%, but their static worker share is 6.64% and remains a
   secondary option.

### Complete launch ledger

Every actual launch below had an owner/editor check of zero before start, ran serially, streamed
the log while active, exited 0, and loaded the expected runtime DLL unless stated otherwise. No
log or generated artifact was deleted. The two wrapper-report errors after the sampler processes
exited did not invalidate their already-written artifacts; the initial worms wrapper error did
not start an Unreal process.

| launch | outcome |
|---|---|
| PerfSampledBuild_ReRank_20260915 | PASS; UBT result succeeded, target up to date. |
| ReRankStaticBaseA_20260915 | PASS; mask 0x00000000, 29.368018 worker s. |
| ReRankStaticBaseB_20260915 | PASS; mask 0x00000000, 29.229716 worker s. |
| ReRankStaticCaveWarpA_20260915 | PASS; mask 0x00000001. |
| ReRankStaticCaveWarpB_20260915 | PASS; mask 0x00000001. |
| ReRankStaticDetailOpsA_20260915 | PASS; mask 0x00000002. |
| ReRankStaticDetailOpsB_20260915 | PASS; mask 0x00000002. |
| ReRankStaticRoomSDFA_20260915 | PASS; mask 0x00000004. |
| ReRankStaticRoomSDFB_20260915 | PASS; mask 0x00000004. |
| ReRankStaticTunnelSDFA_20260915 | PASS; mask 0x00000008. |
| ReRankStaticTunnelSDFB_20260915 | PASS; mask 0x00000008. |
| ReRankStaticTunnelCoreA_20260915 | PASS; mask 0x00000010. |
| ReRankStaticTunnelCoreB_20260915 | PASS; mask 0x00000010. |
| ReRankStaticPassageCarvingA_20260915 | PASS; mask 0x00000020. |
| ReRankStaticPassageCarvingB_20260915 | PASS; mask 0x00000020. |
| ReRankStaticPassageStructuralPostsA_20260915 | PASS; mask 0x00000040. |
| ReRankStaticPassageStructuralPostsB_20260915 | PASS; mask 0x00000040. |
| ReRankStaticNativeFloorA_20260915 | PASS; mask 0x00000080. |
| ReRankStaticNativeFloorB_20260915 | PASS; mask 0x00000080. |
| ReRankStaticDisturbancesA_20260915 | PASS; mask 0x00000100. |
| ReRankStaticDisturbancesB_20260915 | PASS; mask 0x00000100. |
| ReRankStaticOriginSpineA_20260915 | PASS; mask 0x00000200. |
| ReRankStaticOriginSpineB_20260915 | PASS; mask 0x00000200. |
| ReRankStaticBoundarySealA_20260915 | PASS; mask 0x00000400. |
| ReRankStaticBoundarySealB_20260915 | PASS; mask 0x00000400. |
| ReRankStaticLandingPostsA_20260915 | PASS; mask 0x00000800. |
| ReRankStaticLandingPostsB_20260915 | PASS; mask 0x00000800. |
| ReRankStaticXYEdgeSealA_20260915 | PASS; mask 0x00001000. |
| ReRankStaticXYEdgeSealB_20260915 | PASS; mask 0x00001000. |
| ReRankStaticPitChimneySDFA_20260915 | PASS; mask 0x00002000. |
| ReRankStaticPitChimneySDFB_20260915 | PASS; mask 0x00002000. |
| ReRankStaticWormsOffA_20260915 | PASS; WormsForceOff=1, mask 0x00000000. |
| ReRankStaticWormsOffB_20260915 | PASS; WormsForceOff=1, mask 0x00000000. |
| ReRankMovingTunnelCoreBaseA_20260915 | PASS; all-on moving baseline. |
| ReRankMovingTunnelCoreA_20260915 | PASS; mask 0x00000010. |
| ReRankMovingTunnelCoreBaseB_20260915 | PASS; all-on moving baseline. |
| ReRankMovingTunnelCoreB_20260915 | PASS; mask 0x00000010. |
| ReRankMovingTunnelSDFBaseA_20260915 | PASS; all-on moving baseline. |
| ReRankMovingTunnelSDFA_20260915 | PASS; mask 0x00000008. |
| ReRankMovingTunnelSDFBaseB_20260915 | PASS; all-on moving baseline. |
| ReRankMovingTunnelSDFB_20260915 | PASS; mask 0x00000008. |
| ReRankMovingPassageCarvingBaseA_20260915 | PASS; all-on moving baseline. |
| ReRankMovingPassageCarvingA_20260915 | PASS; mask 0x00000020. |
| ReRankMovingPassageCarvingBaseB_20260915 | PASS; all-on moving baseline. |
| ReRankMovingPassageCarvingB_20260915 | PASS; mask 0x00000020. |
| ReRankMovingCaveWarpBaseA_20260915 | PASS; all-on moving baseline. |
| ReRankMovingCaveWarpA_20260915 | PASS; mask 0x00000001. |
| ReRankMovingCaveWarpBaseB_20260915 | PASS; all-on moving baseline. |
| ReRankMovingCaveWarpB_20260915 | PASS; mask 0x00000001. |
| ReRankMovingPassageStructuralPostsBaseA_20260915 | PASS; all-on moving baseline. |
| ReRankMovingPassageStructuralPostsA_20260915 | PASS; mask 0x00000040. |
| ReRankMovingPassageStructuralPostsBaseB_20260915 | PASS; all-on moving baseline. |
| ReRankMovingPassageStructuralPostsB_20260915 | PASS; mask 0x00000040. |
| ReRankSamplerMovingOn_20260915 | PASS process; exit 0, sampler stopped, expected DLL, raw CSV written. Wrapper status print failed after exit on an invalid inline if expression; artifact was independently verified. |
| ReRankSamplerMovingOffline_20260915 | PASS process; exit 0, symbolization completed, expected DLL, summary written. Wrapper post-check made the same invalid inline if mistake; summary was independently verified. |
| initial worms wrapper attempt | FAILED before process start due orchestration-string interpolation; no Unreal process and no measurement. |

All measured sessions were under 30 minutes. No editor was touched. The final checkout remains
without tracked source or asset changes; no commit, push, or stash was performed.
