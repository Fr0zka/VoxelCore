# PERF-SAMPLED

Measurement-only report for the in-process stack sampler, 2026-09-13. No generation, classifier, or field logic was optimized in this round.

## Bottom line

The sampler is useful: it samples registered generation workers without adding scopes to the density hot path, tags every capture with LOD, finds the planted cost, and exposes the game path's dominant work. It is not yet an acceptance-clean low-overhead instrument at the requested default interval. Static timing and worker CPU move beyond the two-run off baseline; moving worker CPU is within noise, but moving request/generation timing is not.

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
