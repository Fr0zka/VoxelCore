// Correctness-round regression coverage for the cold-audit fixes.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/Async.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelBiomeDefinition.h"
#include "VoxelMarchingCubesMesher.h"

namespace
{
    using namespace VoxelForgeTest;

    float FloatFromBits(uint32 Bits)
    {
        float Value = 0.0f;
        FMemory::Memcpy(&Value, &Bits, sizeof(Value));
        return Value;
    }

    bool SameVector(const FVector& A, const FVector& B)
    {
        return BitEqual(A.X, B.X) && BitEqual(A.Y, B.Y) && BitEqual(A.Z, B.Z);
    }

    bool SameVector2D(const FVector2D& A, const FVector2D& B)
    {
        return BitEqual(A.X, B.X) && BitEqual(A.Y, B.Y);
    }

    bool SameMesh(const FVoxelMeshData& A, const FVoxelMeshData& B, FString& OutWhy)
    {
        if (A.NumCeilingTriangles != B.NumCeilingTriangles)
        {
            OutWhy = FString::Printf(TEXT("NumCeilingTriangles %d != %d"),
                                     A.NumCeilingTriangles, B.NumCeilingTriangles);
            return false;
        }

        if (A.Vertices.Num() != B.Vertices.Num()
            || A.Triangles.Num() != B.Triangles.Num()
            || A.UVs.Num() != B.UVs.Num()
            || A.Normals.Num() != B.Normals.Num()
            || A.Colors.Num() != B.Colors.Num())
        {
            OutWhy = FString::Printf(
                TEXT("array sizes differ: V %d/%d T %d/%d UV %d/%d N %d/%d C %d/%d"),
                A.Vertices.Num(), B.Vertices.Num(), A.Triangles.Num(), B.Triangles.Num(),
                A.UVs.Num(), B.UVs.Num(), A.Normals.Num(), B.Normals.Num(),
                A.Colors.Num(), B.Colors.Num());
            return false;
        }

        for (int32 Index = 0; Index < A.Vertices.Num(); ++Index)
        {
            if (!SameVector(A.Vertices[Index], B.Vertices[Index]))
            {
                OutWhy = FString::Printf(TEXT("vertex %d differs"), Index);
                return false;
            }
            if (!SameVector2D(A.UVs[Index], B.UVs[Index]))
            {
                OutWhy = FString::Printf(TEXT("UV %d differs"), Index);
                return false;
            }
            if (!SameVector(A.Normals[Index], B.Normals[Index]))
            {
                OutWhy = FString::Printf(TEXT("normal %d differs"), Index);
                return false;
            }
            if (A.Colors[Index] != B.Colors[Index])
            {
                OutWhy = FString::Printf(TEXT("colour %d differs"), Index);
                return false;
            }
        }
        for (int32 Index = 0; Index < A.Triangles.Num(); ++Index)
        {
            if (A.Triangles[Index] != B.Triangles[Index])
            {
                OutWhy = FString::Printf(TEXT("triangle index %d differs"), Index);
                return false;
            }
        }
        return true;
    }

    float SampleWithTileContext(const UVoxelGenerator& Generator, int32 Z,
                                int32 Step, int32 Cells, bool* bOutCacheContext = nullptr)
    {
        TGuardValue<int32> StepGuard(VoxelGenLOD::SampleStep, Step);
        TGuardValue<FIntVector> OriginGuard(
            VoxelGenLOD::TileOriginVoxels, FIntVector::ZeroValue);
        TGuardValue<int32> CellsGuard(VoxelGenLOD::TileCellsPerAxis, Cells);
        if (bOutCacheContext)
        {
            FIntVector ContextOrigin;
            int32 ContextStep = 1;
            int32 ContextCells = 0;
            *bOutCacheContext = VoxelGenLOD::IsTileCacheWindowEnabled(false)
                && VoxelGenLOD::GetThreadTileCacheWindow(
                    ContextOrigin, ContextStep, ContextCells);
        }
        return Generator.GetDensityAt(0.0f, 0.0f, static_cast<float>(Z));
    }

    bool IsFiniteGenerationParams(const FStrateGenerationParams& Params)
    {
        return VoxelMath::IsFinite(Params.BaseDensity)
            && VoxelMath::IsFinite(Params.VerticalScale)
            && VoxelMath::IsFinite(Params.WormFrequency)
            && VoxelMath::IsFinite(Params.WormThreshold)
            && VoxelMath::IsFinite(Params.WormStrength)
            && VoxelMath::IsFinite(Params.SurfaceRoughness)
            && VoxelMath::IsFinite(Params.BoundarySealThickness)
            && VoxelMath::IsFinite(Params.StrateTopWorldZ)
            && VoxelMath::IsFinite(Params.StrateBottomWorldZ);
    }
}

//=============================================================================
// #7 — tile-cache context switch and moved-from tunnel state
//=============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeTileCacheContextDeterminismTest,
    "VoxelForge.Determinism.TileCacheContextSwitch",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeTileCacheContextDeterminismTest::RunTest(const FString& Parameters)
{
    FTestWorld World;
    World.Build(/*Seed*/1337, /*GapChunks*/2, /*bUseOperatorStack*/true);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }

    const FIntVector OriginChunk = FIntVector::ZeroValue;
    AddInfo(FString::Printf(
        TEXT("#7 fixture origin chunk: type=%d uses_op_stack=%d z_span=[%d,%d]."),
        static_cast<int32>(World.StrateManager->GetGeneratorTypeForChunk(OriginChunk)),
        World.StrateManager->UsesOperatorStackForChunk(OriginChunk) ? 1 : 0,
        World.BottomChunkZ, World.TopChunkZ));

    const UVoxelGenerator* Generator = World.Generator.Get();
    const int32 SampleZ[] = { 0, 8, 16, 24, 31 };
    TArray<float> CoarseContext;
    TArray<float> FineContext;
    bool bCoarseCacheContext = false;
    bool bFineCacheContext = false;
    CoarseContext.Reserve(UE_ARRAY_COUNT(SampleZ));
    FineContext.Reserve(UE_ARRAY_COUNT(SampleZ));

    // Step 16/Cells 16 enables the op-stack tile-window context. Step 1 disables it. The
    // point remains in chunk (0,0,0), including the origin column that shares a Z cache entry.
    for (const int32 Z : SampleZ)
    {
        CoarseContext.Add(SampleWithTileContext(
            *Generator, Z, 16, 16, &bCoarseCacheContext));
    }
    for (const int32 Z : SampleZ)
    {
        FineContext.Add(SampleWithTileContext(
            *Generator, Z, 1, 32, &bFineCacheContext));
    }

    TArray<int32> SampleZCopy;
    for (const int32 Z : SampleZ) { SampleZCopy.Add(Z); }
    TFuture<TArray<float>> FreshFuture = Async(EAsyncExecution::Thread,
        [Generator, SampleZCopy = MoveTemp(SampleZCopy)]() mutable
        {
            TArray<float> Result;
            Result.Reserve(SampleZCopy.Num());
            for (const int32 Z : SampleZCopy)
            {
                Result.Add(Generator->GetDensityAt(0.0f, 0.0f, static_cast<float>(Z)));
            }
            return Result;
        });
    const TArray<float> FreshThread = FreshFuture.Get();

    int32 CoarseMismatches = 0;
    int32 FineMismatches = 0;
    bool bObservedNonZero = false;
    for (int32 Index = 0; Index < FreshThread.Num(); ++Index)
    {
        bObservedNonZero |= FMath::Abs(FreshThread[Index]) > KINDA_SMALL_NUMBER;
        if (!BitEqual(CoarseContext[Index], FreshThread[Index])) { ++CoarseMismatches; }
        if (!BitEqual(FineContext[Index], FreshThread[Index])) { ++FineMismatches; }
    }

    TestTrue(TEXT("the origin-column samples exercise a non-empty operator-stack field"),
             bObservedNonZero);
    TestEqual(TEXT("coarse tile-window context matches a fresh thread"), CoarseMismatches, 0);
    TestEqual(TEXT("switching to the non-window context matches a fresh thread"),
              FineMismatches, 0);
    AddInfo(FString::Printf(
        TEXT("#7 origin-column context switch: %d samples, coarse mismatches=%d, fine mismatches=%d; "
             "all points are in chunk (0,0,0), cache_context coarse=%d fine=%d."),
        FreshThread.Num(), CoarseMismatches, FineMismatches,
        bCoarseCacheContext ? 1 : 0, bFineCacheContext ? 1 : 0));

    // Exercise the game-path ordering: a coarse tile warms the moved-out shared entry, then a
    // level-0 tile at the same origin is compared with a fresh world's level-0 output.
    FTestWorld WarmWorld;
    FTestWorld FreshWorld;
    WarmWorld.Build(/*Seed*/1337, /*GapChunks*/2, /*bUseOperatorStack*/true);
    FreshWorld.Build(/*Seed*/1337, /*GapChunks*/2, /*bUseOperatorStack*/true);
    if (!WarmWorld.IsValid() || !FreshWorld.IsValid())
    {
        AddError(WarmWorld.IsValid() && FreshWorld.IsValid()
            ? TEXT("Unexpected per-tile fixture state")
            : (!WarmWorld.IsValid() ? WarmWorld.WhyInvalid() : FreshWorld.WhyInvalid()));
        return false;
    }

    TStrongObjectPtr<UVoxelMarchingCubesMesher> WarmMesher(
        NewObject<UVoxelMarchingCubesMesher>(GetTransientPackage(), NAME_None, RF_Transient));
    TStrongObjectPtr<UVoxelMarchingCubesMesher> FreshMesher(
        NewObject<UVoxelMarchingCubesMesher>(GetTransientPackage(), NAME_None, RF_Transient));
    WarmMesher->SetGenerator(WarmWorld.Generator.Get());
    FreshMesher->SetGenerator(FreshWorld.Generator.Get());

    int64 CoarseSamples = 0;
    WarmMesher->GenerateMesh(FIntVector::ZeroValue, 16, 16, nullptr,
                             INT32_MIN, INT32_MAX, &CoarseSamples);
    int64 WarmFineSamples = 0;
    int64 FreshFineSamples = 0;
    const FVoxelMeshData WarmFine = WarmMesher->GenerateMesh(
        FIntVector::ZeroValue, 1, CHUNK_SIZE, nullptr, INT32_MIN, INT32_MAX,
        &WarmFineSamples);
    const FVoxelMeshData FreshFine = FreshMesher->GenerateMesh(
        FIntVector::ZeroValue, 1, CHUNK_SIZE, nullptr, INT32_MIN, INT32_MAX,
        &FreshFineSamples);

    TestTrue(TEXT("the per-tile identity check actually meshed the level-0 origin tile"),
             WarmFineSamples > 0 && FreshFineSamples > 0);
    FString MeshDifference;
    TestTrue(TEXT("coarse-then-fine game path equals a fresh fine tile"),
             SameMesh(WarmFine, FreshFine, MeshDifference));
    if (!MeshDifference.IsEmpty())
    {
        AddInfo(FString::Printf(TEXT("Per-tile difference detail: %s"), *MeshDifference));
    }
    AddInfo(FString::Printf(
        TEXT("#7 game-path tile identity: coarse samples=%lld, warm fine=%lld, fresh fine=%lld, "
             "warm vertices=%d, fresh vertices=%d."),
        (long long)CoarseSamples, (long long)WarmFineSamples, (long long)FreshFineSamples,
        WarmFine.Vertices.Num(), FreshFine.Vertices.Num()));
    return true;
}

//=============================================================================
// #3 — diff-layer identity in the worker-local snapshot slots
//=============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeDiffLayerIdentityTest,
    "VoxelForge.Determinism.DiffLayerIdentity",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeDiffLayerIdentityTest::RunTest(const FString& Parameters)
{
    FTestWorld CarvedWorld;
    FTestWorld FilledWorld;
    CarvedWorld.Build();
    FilledWorld.Build();
    if (!CarvedWorld.IsValid() || !FilledWorld.IsValid())
    {
        AddError(CarvedWorld.IsValid() && FilledWorld.IsValid()
            ? TEXT("Unexpected diff identity fixture state")
            : (!CarvedWorld.IsValid() ? CarvedWorld.WhyInvalid() : FilledWorld.WhyInvalid()));
        return false;
    }

    const FVector P(0.0f, 0.0f, 0.0f);
    FVoxelModification Carve;
    Carve.Center = P;
    Carve.Radius = 8.0f;
    Carve.Strength = -64.0f;
    Carve.Shape = EVoxelBrushShape::Sphere;
    FVoxelModification Fill = Carve;
    Fill.Strength = 64.0f;

    TestTrue(TEXT("the carve layer accepted its edit"),
             CarvedWorld.DiffLayer->ApplyModification(Carve).Num() > 0);
    TestTrue(TEXT("the fill layer accepted its edit"),
             FilledWorld.DiffLayer->ApplyModification(Fill).Num() > 0);
    TestEqual(TEXT("the two different layers have the same chunk/version key"),
              CarvedWorld.DiffLayer->GetModsVersion(), FilledWorld.DiffLayer->GetModsVersion());
    TestTrue(TEXT("the two layers have distinct worker-cache identities"),
             CarvedWorld.DiffLayer->GetCacheLifetimeId()
                 != FilledWorld.DiffLayer->GetCacheLifetimeId());

    // Warm the carve snapshot, then sample the fill on the same thread. A cache keyed only by
    // (chunk, version) returns the carve's list for the fill layer here.
    const float Carved = CarvedWorld.Generator->GetDensityAt(P.X, P.Y, P.Z);
    const float Filled = FilledWorld.Generator->GetDensityAt(P.X, P.Y, P.Z);

    const UVoxelGenerator* FilledGenerator = FilledWorld.Generator.Get();
    TFuture<float> FreshFilledFuture = Async(EAsyncExecution::Thread,
        [FilledGenerator, P]()
        {
            return FilledGenerator->GetDensityAt(P.X, P.Y, P.Z);
        });
    const float FreshFilled = FreshFilledFuture.Get();

    TestTrue(TEXT("the two edits produce different density values at their common centre"),
             !BitEqual(Carved, FreshFilled));
    TestTrue(TEXT("the second layer does not inherit the first layer's snapshot"),
             BitEqual(Filled, FreshFilled));
    AddInfo(FString::Printf(
        TEXT("#3 same-thread diff snapshots: carve=%.9g [0x%08X], fill=%.9g [0x%08X], "
             "fresh fill=%.9g [0x%08X]."),
        Carved, *reinterpret_cast<const uint32*>(&Carved), Filled,
        *reinterpret_cast<const uint32*>(&Filled), FreshFilled,
        *reinterpret_cast<const uint32*>(&FreshFilled)));
    return true;
}

//=============================================================================
// #8 — surface/biome worker-cache identities
//=============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeSurfaceBiomeCacheIdentityTest,
    "VoxelForge.Determinism.SurfaceBiomeCacheIdentity",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeSurfaceBiomeCacheIdentityTest::RunTest(const FString& Parameters)
{
    FTestWorld A;
    FTestWorld B;
    A.Build();
    B.Build();
    if (!A.IsValid() || !B.IsValid())
    {
        AddError(A.IsValid() && B.IsValid()
            ? TEXT("Unexpected surface cache fixture state")
            : (!A.IsValid() ? A.WhyInvalid() : B.WhyInvalid()));
        return false;
    }

    // The fixture normally gives each world a unique layout version. Bring A forward by one
    // rebuild so the two managers intentionally share (seed, chunk, layout version); only their
    // process-unique owner identities may distinguish their worker caches.
    A.Reinitialize();
    TestEqual(TEXT("the cache-collision worlds intentionally share a layout version"),
              A.StrateManager->GetLayoutVersion(), B.StrateManager->GetLayoutVersion());
    TestTrue(TEXT("the cache-collision worlds have distinct manager identities"),
             A.StrateManager->GetCacheLifetimeId() != B.StrateManager->GetCacheLifetimeId());
    TestTrue(TEXT("the cache-collision worlds have distinct generator instances"),
             A.Generator.Get() != B.Generator.Get());

    int32 SurfTop = 0, SurfBottom = 0;
    if (!A.GetSlotVoxelZRange(FTestWorld::SlotSurfaceWorld, SurfTop, SurfBottom))
    {
        AddError(TEXT("The fixture has no SurfaceWorld slot."));
        return false;
    }
    const int32 SurfChunkZ = FMath::FloorToInt(
        static_cast<float>(SurfTop + SurfBottom) * 0.5f / CHUNK_SIZE);

    UVoxelStrateDefinition* BSurface = B.Definitions[FTestWorld::SlotSurfaceWorld].Get();
    BSurface->SurfaceParams.ElevationRange *= 3.0f;
    BSurface->SurfaceParams.MountainStrength = 1.0f;
    BSurface->SurfaceParams.ContinentFrequency *= 1.7f;
    BSurface->SurfaceParams.BaseGroundRelative = FMath::Clamp(
        BSurface->SurfaceParams.BaseGroundRelative + 0.12f, 0.0f, 1.0f);

    bool bFoundDifferentSurface = false;
    float FirstX = 0.0f, FirstY = 0.0f, HeightA = 0.0f, HeightB = 0.0f;
    for (int32 Index = 0; Index < 96 && !bFoundDifferentSurface; ++Index)
    {
        const float X = 180.0f + static_cast<float>(Index) * 23.0f;
        const float Y = -260.0f + static_cast<float>(Index) * 19.0f;
        float CeilA = 0.0f, CeilB = 0.0f;
        if (!A.Generator->GetSurfaceHeightAt(X, Y, SurfChunkZ, HeightA, CeilA)
            || !B.Generator->GetSurfaceHeightAt(X, Y, SurfChunkZ, HeightB, CeilB))
        {
            continue;
        }
        if (FMath::Abs(HeightA - HeightB) > 1.0f)
        {
            bFoundDifferentSurface = true;
            FirstX = X;
            FirstY = Y;
        }
    }
    TestTrue(TEXT("the two surface definitions produce a distinct heightfield"),
             bFoundDifferentSurface);
    if (!bFoundDifferentSurface) { return false; }

    // Warm A immediately before B for each probe. Before the owner/manager key fix, B's integer
    // XY column lookup reuses A's column because all former key fields are deliberately equal.
    int32 NumBatches = 0;
    int32 NumBStale = 0;
    int32 NumAReferenceDifference = 0;
    for (int32 Offset = -12; Offset <= 12; ++Offset)
    {
        const float Z = FMath::FloorToFloat((HeightA + HeightB) * 0.5f)
                      + static_cast<float>(Offset);
        const float ValueA = A.Generator->GetDensityAt(FirstX, FirstY, Z);
        const float ValueB = B.Generator->GetDensityAt(FirstX, FirstY, Z);
        const UVoxelGenerator* BGenerator = B.Generator.Get();
        TFuture<float> FreshFuture = Async(EAsyncExecution::Thread,
            [BGenerator, FirstX, FirstY, Z]()
            {
                return BGenerator->GetDensityAt(FirstX, FirstY, Z);
            });
        const float FreshB = FreshFuture.Get();
        ++NumBatches;
        if (!BitEqual(ValueB, FreshB)) { ++NumBStale; }
        if (!BitEqual(ValueA, FreshB)) { ++NumAReferenceDifference; }
    }

    TestTrue(TEXT("the surface probe includes a distinct A/B field value"),
             NumAReferenceDifference > 0);
    TestEqual(TEXT("the surface cache never returns the other world's column"), NumBStale, 0);

    // Exercise the biome grid and vertex-palette path with the same intentionally colliding
    // layout key. One full-climate biome per world makes the expected palette unambiguous.
    TStrongObjectPtr<UVoxelBiomeDefinition> BiomeA(
        NewObject<UVoxelBiomeDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
    TStrongObjectPtr<UVoxelBiomeDefinition> BiomeB(
        NewObject<UVoxelBiomeDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
    BiomeA->ReliefMin = 0.0f; BiomeA->ReliefMax = 1.0f;
    BiomeA->MoistureMin = 0.0f; BiomeA->MoistureMax = 1.0f;
    BiomeA->MaterialPaletteIndex = 17;
    BiomeB->ReliefMin = 0.0f; BiomeB->ReliefMax = 1.0f;
    BiomeB->MoistureMin = 0.0f; BiomeB->MoistureMax = 1.0f;
    BiomeB->MaterialPaletteIndex = 91;
    A.Definitions[FTestWorld::SlotSurfaceWorld]->Biomes.Add(BiomeA.Get());
    B.Definitions[FTestWorld::SlotSurfaceWorld]->Biomes.Add(BiomeB.Get());

    int32 PaletteA = 0, PaletteAN = 0;
    int32 PaletteB = 0, PaletteBN = 0;
    float BlendA = 0.0f, BlendB = 0.0f;
    A.Generator->GetBiomeMaterialAt(FirstX, FirstY, static_cast<float>(SurfTop),
                                     PaletteA, PaletteAN, BlendA);
    B.Generator->GetBiomeMaterialAt(FirstX, FirstY, static_cast<float>(SurfTop),
                                     PaletteB, PaletteBN, BlendB);
    TestEqual(TEXT("world A resolves its own biome palette"), PaletteA, 17);
    TestEqual(TEXT("world B resolves its own biome palette"), PaletteB, 91);
    TestEqual(TEXT("world A's neighbour palette is its own palette"), PaletteAN, 17);
    TestEqual(TEXT("world B's neighbour palette is its own palette"), PaletteBN, 91);

    // Public cache contract: the identity fields are part of Contains(), not just a comment or
    // a caller convention. This also remains a cheap unit check for future cache refactors.
    FChunkBiomeCache Cache;
    Cache.ValidMinX = -10.0f; Cache.ValidMaxX = 10.0f;
    Cache.ValidMinY = -10.0f; Cache.ValidMaxY = 10.0f;
    Cache.ChunkZ = SurfChunkZ; Cache.Seed = 1337;
    Cache.OwnerId = 101; Cache.ManagerLifetimeId = 202;
    TestTrue(TEXT("biome cache accepts the exact owner and manager identity"),
             Cache.Contains(0.0f, 0.0f, SurfChunkZ, 1337, 101, 202));
    TestFalse(TEXT("biome cache rejects a different generator identity"),
              Cache.Contains(0.0f, 0.0f, SurfChunkZ, 1337, 102, 202));
    TestFalse(TEXT("biome cache rejects a different manager identity"),
              Cache.Contains(0.0f, 0.0f, SurfChunkZ, 1337, 101, 203));
    return true;
}

//=============================================================================
// #14 — passage-version invalidation when the new layout is empty
//=============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeEmptyPassageVersionTest,
    "VoxelForge.Determinism.EmptyPassageVersion",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeEmptyPassageVersionTest::RunTest(const FString& Parameters)
{
    FTestWorld World;
    World.Build();
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }

    // Prime the worker-local paths that keep nearby-passage shortlists, then rebuild to an empty
    // layout. The old early return cleared Passages but did not bump PassagesVersion.
    World.Generator->ClassifyTile(FIntVector::ZeroValue, 1, 8);
    const uint32 Before = World.StrateManager->GetLayoutVersion();
    World.Settings->TotalStrates = 0;
    World.Settings->StratePool.Reset();
    World.Settings->FixedStrates.Reset();
    const bool bInitialized = World.StrateManager->Initialize(World.Settings.Get(), World.Settings->Seed);

    TestTrue(TEXT("empty settings still complete the empty layout rebuild"), bInitialized);
    TestEqual(TEXT("the rebuilt layout is empty"), World.StrateManager->GetNumStrates(), 0);
    TestTrue(TEXT("clearing passages bumps the worker invalidation version"),
             World.StrateManager->GetLayoutVersion() != Before);
    TestEqual(TEXT("the empty layout has no passages"), World.StrateManager->GetPassages().Num(), 0);
    AddInfo(FString::Printf(TEXT("#14 passage version %u -> %u on empty rebuild."),
                            Before, World.StrateManager->GetLayoutVersion()));
    return true;
}

//=============================================================================
// #20 — zero blend is a valid runtime value and must remain finite
//=============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeZeroTransitionBlendTest,
    "VoxelForge.Determinism.ZeroTransitionBlend",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeZeroTransitionBlendTest::RunTest(const FString& Parameters)
{
    FTestWorld World;
    World.Build();
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }
    for (int32 Index = 0; Index < World.Definitions.Num(); ++Index)
    {
        UVoxelStrateDefinition* Definition = World.Definitions[Index].Get();
        Definition->TransitionBlendChunks = 0;
        Definition->TransitionType = (Index & 1) == 0
            ? EVoxelStrateTransition::Gradient : EVoxelStrateTransition::Interleaved;
    }
    World.Reinitialize();

    int32 NumQueries = 0;
    int32 NumNonFinite = 0;
    for (const FStrateSlot& Slot : World.StrateManager->GetLayout())
    {
        const int32 BoundaryChunks[] = { Slot.BottomChunkZ, Slot.TopChunkZ };
        for (const int32 ChunkZ : BoundaryChunks)
        {
            const FStrateGenerationParams Params =
                World.StrateManager->GetGenerationParams(FIntVector(0, 0, ChunkZ));
            ++NumQueries;
            if (!IsFiniteGenerationParams(Params)) { ++NumNonFinite; }
        }
    }
    TestEqual(TEXT("zero-blend generation parameters remain finite at every boundary"),
              NumNonFinite, 0);
    AddInfo(FString::Printf(TEXT("#20 queried %d zero-blend boundary parameter sets."), NumQueries));
    return true;
}

//=============================================================================
// #15 and #21 — malformed API input and direct per-chunk range lookup
//=============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeDiffApiBoundaryTest,
    "VoxelForge.Determinism.DiffApiBoundary",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeDiffApiBoundaryTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UVoxelDiffLayer> Diff(
        NewObject<UVoxelDiffLayer>(GetTransientPackage(), NAME_None, RF_Transient));
    Diff->SetBudget(0, 50.0f, 0.0f);

    const float NaN = FloatFromBits(0x7FC12345u);
    const float Inf = FloatFromBits(0x7F800000u);
    TestFalse(TEXT("CanModify rejects zero radius"), Diff->CanModify(0.0f));
    TestFalse(TEXT("CanModify rejects NaN radius"), Diff->CanModify(NaN));
    TestFalse(TEXT("CanModify rejects infinity radius"), Diff->CanModify(Inf));

    auto ExpectRejected = [&](const TCHAR* Label, const FVoxelModification& Mod)
    {
        const int32 Before = Diff->GetTotalModificationCount();
        const TArray<FIntVector> Touched = Diff->ApplyModification(Mod);
        TestEqual(FString::Printf(TEXT("%s returns no affected chunks"), Label), Touched.Num(), 0);
        TestEqual(FString::Printf(TEXT("%s does not update stored entries"), Label),
                  Diff->GetTotalModificationCount(), Before);
    };

    FVoxelModification Mod;
    Mod.Radius = 0.0f;
    ExpectRejected(TEXT("zero radius"), Mod);
    Mod = FVoxelModification();
    Mod.Radius = NaN;
    ExpectRejected(TEXT("NaN radius"), Mod);
    Mod = FVoxelModification();
    Mod.Center.X = Inf;
    ExpectRejected(TEXT("infinite centre"), Mod);
    Mod = FVoxelModification();
    Mod.Strength = NaN;
    ExpectRejected(TEXT("NaN strength"), Mod);
    Mod = FVoxelModification();
    Mod.Shape = EVoxelBrushShape::Box;
    Mod.BoxExtent.X = -1.0f;
    ExpectRejected(TEXT("negative box extent"), Mod);
    Mod = FVoxelModification();
    Mod.Shape = EVoxelBrushShape::Capsule;
    Mod.CapsuleEnd.Y = Inf;
    ExpectRejected(TEXT("infinite capsule endpoint"), Mod);
    Mod = FVoxelModification();
    Mod.Shape = static_cast<EVoxelBrushShape>(255);
    ExpectRejected(TEXT("unknown brush shape"), Mod);

    // Shape-aware budget regression: the old implementation billed both of these as a sphere using
    // Radius alone, so each incorrectly fit under a 1,000-voxel cumulative budget.
    TStrongObjectPtr<UVoxelDiffLayer> ShapeBudget(
        NewObject<UVoxelDiffLayer>(GetTransientPackage(), NAME_None, RF_Transient));
    ShapeBudget->SetBudget(0, 50.0f, 1000.0f);
    FVoxelModification LongCapsule;
    LongCapsule.Shape = EVoxelBrushShape::Capsule;
    LongCapsule.Center = FVector::ZeroVector;
    LongCapsule.CapsuleEnd = FVector(0.0f, 1000.0f, 0.0f);
    LongCapsule.Radius = 3.0f;
    LongCapsule.Falloff = 2.0f;
    TestFalse(TEXT("a long capsule is charged for its swept volume"),
              ShapeBudget->CanModify(LongCapsule));
    TestEqual(TEXT("the long capsule is not stored under the shape budget"),
              ShapeBudget->ApplyModification(LongCapsule).Num(), 0);

    FVoxelModification LargeBox;
    LargeBox.Shape = EVoxelBrushShape::Box;
    LargeBox.BoxExtent = FVector(5.0f);
    LargeBox.Radius = 5.0f;
    TestFalse(TEXT("a box is charged for its expanded box volume"),
              ShapeBudget->CanModify(LargeBox));
    TestEqual(TEXT("the large box is not stored under the shape budget"),
              ShapeBudget->ApplyModification(LargeBox).Num(), 0);

    FVoxelModification Valid;
    Valid.Center = FVector(3.0f * CHUNK_SIZE + 4.0f,
                           4.0f * CHUNK_SIZE + 4.0f, 2.0f);
    Valid.Radius = 3.0f;
    Valid.Strength = -4.0f;
    TestTrue(TEXT("a valid modification remains accepted after malformed inputs"),
             Diff->ApplyModification(Valid).Num() > 0);

    const FIntVector Hit(3, 4, 0);
    TestTrue(TEXT("range lookup finds the directly grouped affected chunk"),
             Diff->HasAnyModInChunkRange(Hit, Hit));
    TestFalse(TEXT("range lookup rejects a far unmodified chunk"),
              Diff->HasAnyModInChunkRange(FIntVector(30, 30, 30), FIntVector(30, 30, 30)));
    TestFalse(TEXT("range lookup rejects reversed bounds"),
              Diff->HasAnyModInChunkRange(FIntVector(4, 4, 0), FIntVector(3, 4, 0)));
    TestTrue(TEXT("the valid edit left a non-empty diff layer"), Diff->HasAnyMods());
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
