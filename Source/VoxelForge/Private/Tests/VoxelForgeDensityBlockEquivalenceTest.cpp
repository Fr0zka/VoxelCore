#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "HAL/IConsoleManager.h"
#include "VoxelClipmapDesiredTiles.h"
#include "VoxelDensityOpStack.h"
#include "VoxelForgeTestFixture.h"

namespace
{
    // The marching-cubes grid contains 32 cells, with one sample of halo on each side.
    constexpr int32 MesherGridSize = 35;
    constexpr int32 ArchetypeCount = 8;
    constexpr int32 PassageGridCount = 2;
    constexpr int32 GridSetCount = ArchetypeCount + PassageGridCount;

    static bool SameFloatBits(float A, float B)
    {
        return FMemory::Memcmp(&A, &B, sizeof(float)) == 0;
    }

    static FIntVector MakeMesherGridOrigin(const FIntVector& TargetChunk, int32 Level)
    {
        const int32 TileSpanChunks = 1 << Level;
        const FIntVector TileCoord = VoxelClipmapDesiredTiles::FloorDiv(
            TargetChunk, TileSpanChunks);
        const int32 Step = 1 << Level;
        return TileCoord * (CHUNK_SIZE << Level) - FIntVector(Step, Step, Step);
    }

    struct FGridComparison
    {
        int64 Samples = 0;
        int32 Mismatches = 0;
        FString FirstMismatch;
        TArray<float> BlockDensities;
    };

    static bool BuildProductionStackForArchetype(
        VoxelForgeTest::FTestWorld& World, ECaveGeneratorType Type,
        const FIntVector& ChunkCoord, FVoxelOpStack& OutStack, FVoxelOpContext& OutContext)
    {
        const int32 Seed = World.Settings->GetEffectiveWorldSeed();
        const float SpineRadius = World.Settings->GetEffectiveOriginSpineRadius();
        const UVoxelStrateManager* Manager = World.StrateManager.Get();
        OutContext = FVoxelOpContext();
        OutContext.ChunkCoord = ChunkCoord;
        OutContext.Seed = static_cast<uint32>(Seed);
        OutContext.LayoutVersion = Manager->GetLayoutVersion();
        OutContext.WorldRadiusVoxels = World.Settings->GetEffectiveWorldRadiusVoxels();
        OutContext.EdgeSealThickness = World.Settings->EdgeSealThickness;

        switch (Type)
        {
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
        {
            const FSlabGenerationParams P = Manager->GetSlabParamsForChunk(ChunkCoord);
            OutContext.StrateTopWorldZ = P.StrateTopWorldZ;
            OutContext.StrateBottomWorldZ = P.StrateBottomWorldZ;
            VoxelDensityOps::BuildSlabStack(OutStack, P, Seed, SpineRadius, Manager);
            return true;
        }
        case ECaveGeneratorType::Maze:
        {
            const FMazeGenerationParams P = Manager->GetMazeParamsForChunk(ChunkCoord);
            OutContext.StrateTopWorldZ = P.StrateTopWorldZ;
            OutContext.StrateBottomWorldZ = P.StrateBottomWorldZ;
            VoxelDensityOps::BuildMazeStack(OutStack, P, Seed, SpineRadius, Manager);
            return true;
        }
        case ECaveGeneratorType::SurfaceWorld:
        {
            const FSurfaceGenerationParams P = Manager->GetSurfaceParamsForChunk(ChunkCoord);
            OutContext.StrateTopWorldZ = P.StrateTopWorldZ;
            OutContext.StrateBottomWorldZ = P.StrateBottomWorldZ;
            // The generator-grid comparison below covers the resolved biome route. This builder
            // exercises the production stack's base surface branch directly.
            VoxelDensityOps::BuildSurfaceStack(
                OutStack, P, Seed, SpineRadius, Manager, TArray<FSurfaceGenerationParams>(), nullptr);
            return true;
        }
        case ECaveGeneratorType::VerticalShafts:
        {
            const FVerticalShaftParams P = Manager->GetVerticalShaftParamsForChunk(ChunkCoord);
            OutContext.StrateTopWorldZ = P.StrateTopWorldZ;
            OutContext.StrateBottomWorldZ = P.StrateBottomWorldZ;
            VoxelDensityOps::BuildVerticalShaftStack(OutStack, P, Seed, SpineRadius, Manager);
            return true;
        }
        case ECaveGeneratorType::FloatingIslands:
        {
            const FFloatingIslandParams P = Manager->GetFloatingIslandParamsForChunk(ChunkCoord);
            OutContext.StrateTopWorldZ = P.StrateTopWorldZ;
            OutContext.StrateBottomWorldZ = P.StrateBottomWorldZ;
            VoxelDensityOps::BuildFloatingIslandStack(OutStack, P, Seed, SpineRadius, Manager);
            return true;
        }
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
        {
            const FStrateGenerationParams P = Manager->GetGenerationParams(ChunkCoord);
            OutContext.StrateTopWorldZ = P.StrateTopWorldZ;
            OutContext.StrateBottomWorldZ = P.StrateBottomWorldZ;
            VoxelDensityOps::BuildTunnelNetworkStack(OutStack, P, Seed, SpineRadius, Manager);
            return true;
        }
        default:
            return false;
        }
    }

    static bool FindChunkSampleRange(int32 AxisOrigin, int32 Step, int32 AxisSize,
                                     int32 TargetChunk, int32& OutMin, int32& OutMax)
    {
        OutMin = INT32_MAX;
        OutMax = INT32_MIN;
        for (int32 Index = 0; Index < AxisSize; ++Index)
        {
            const int32 Coordinate = AxisOrigin + Index * Step;
            if (FMath::FloorToInt(static_cast<float>(Coordinate) / CHUNK_SIZE) == TargetChunk)
            {
                OutMin = FMath::Min(OutMin, Index);
                OutMax = FMath::Max(OutMax, Index);
            }
        }
        return OutMin != INT32_MAX;
    }

    static FGridComparison CompareStackChunkBlockWithScalar(
        FVoxelOpStack& Stack, FIntVector GridOrigin, int32 Step,
        const FIntVector& TargetChunk)
    {
        FGridComparison Result;
        int32 MinX = 0, MaxX = 0, MinY = 0, MaxY = 0, MinZ = 0, MaxZ = 0;
        if (!FindChunkSampleRange(GridOrigin.X, Step, MesherGridSize, TargetChunk.X, MinX, MaxX)
            || !FindChunkSampleRange(GridOrigin.Y, Step, MesherGridSize, TargetChunk.Y, MinY, MaxY)
            || !FindChunkSampleRange(GridOrigin.Z, Step, MesherGridSize, TargetChunk.Z, MinZ, MaxZ))
        {
            Result.FirstMismatch = TEXT("mesher grid has no samples in the stack's target chunk");
            return Result;
        }

        const int32 SizeX = MaxX - MinX + 1;
        const int32 SizeY = MaxY - MinY + 1;
        const int32 SizeZ = MaxZ - MinZ + 1;
        const FIntVector Origin = GridOrigin + FIntVector(MinX * Step, MinY * Step, MinZ * Step);
        const FIntVector Last = Origin + FIntVector(
            (SizeX - 1) * Step, (SizeY - 1) * Step, (SizeZ - 1) * Step);
        const FBox Box(
            FVector((float)Origin.X, (float)Origin.Y, (float)Origin.Z),
            FVector((float)Last.X, (float)Last.Y, (float)Last.Z));

        TArray<int32> ActiveOps;
        const int32 ActiveCount = Stack.BuildActiveOpList(Box, Step, Origin, ActiveOps);
        TArray<FVoxelOpSample> Samples;
        Samples.SetNumUninitialized(SizeX * SizeY * SizeZ);
        for (FVoxelOpSample& Sample : Samples) { Sample = FVoxelOpSample(); }

        FVoxelOpBlock Block;
        Block.OriginVoxels = Origin;
        Block.Step = Step;
        Block.SizeX = SizeX;
        Block.SizeY = SizeY;
        Block.SizeZ = SizeZ;
        Block.Samples = Samples.GetData();
        Stack.EvalBlock(Block, ActiveOps);

        for (int32 Z = 0; Z < SizeZ; ++Z)
        for (int32 Y = 0; Y < SizeY; ++Y)
        for (int32 X = 0; X < SizeX; ++X)
        {
            const FIntVector SamplePos = Origin + FIntVector(X * Step, Y * Step, Z * Step);
            const float BlockDensity = Block.At(X, Y, Z).Density;
            const float ScalarDensity = Stack.EvalInternal(
                (float)SamplePos.X, (float)SamplePos.Y, (float)SamplePos.Z);
            Result.BlockDensities.Add(BlockDensity);
            ++Result.Samples;
            if (!SameFloatBits(BlockDensity, ScalarDensity))
            {
                ++Result.Mismatches;
                if (Result.FirstMismatch.IsEmpty())
                {
                    Result.FirstMismatch = FString::Printf(
                        TEXT("box=[(%d,%d,%d)..(%d,%d,%d)] active_ops=%d/%d sample=(%d,%d,%d) block=%.9g scalar=%.9g delta=%.9g"),
                        Origin.X, Origin.Y, Origin.Z, Last.X, Last.Y, Last.Z,
                        ActiveCount, Stack.Num(), SamplePos.X, SamplePos.Y, SamplePos.Z,
                        BlockDensity, ScalarDensity, BlockDensity - ScalarDensity);
                }
            }
        }
        return Result;
    }

    static FGridComparison CompareGeneratorBlockWithScalar(
        UVoxelGenerator& Generator, FIntVector Origin, int32 Step)
    {
        FGridComparison Result;
        const int32 Count = MesherGridSize * MesherGridSize * MesherGridSize;
        Result.BlockDensities.SetNumUninitialized(Count);
        Generator.BeginDensityBlock(Origin, Step, MesherGridSize, MesherGridSize, MesherGridSize);
        int32 Index = 0;
        for (int32 Z = 0; Z < MesherGridSize; ++Z)
        for (int32 Y = 0; Y < MesherGridSize; ++Y)
        for (int32 X = 0; X < MesherGridSize; ++X, ++Index)
        {
            const FIntVector SamplePos = Origin + FIntVector(X * Step, Y * Step, Z * Step);
            Result.BlockDensities[Index] = Generator.GetDensityAt(
                (float)SamplePos.X, (float)SamplePos.Y, (float)SamplePos.Z);
        }
        Generator.EndDensityBlock();

        Index = 0;
        for (int32 Z = 0; Z < MesherGridSize; ++Z)
        for (int32 Y = 0; Y < MesherGridSize; ++Y)
        for (int32 X = 0; X < MesherGridSize; ++X, ++Index)
        {
            const FIntVector SamplePos = Origin + FIntVector(X * Step, Y * Step, Z * Step);
            const float ScalarDensity = Generator.GetDensityAt(
                (float)SamplePos.X, (float)SamplePos.Y, (float)SamplePos.Z);
            ++Result.Samples;
            if (!SameFloatBits(Result.BlockDensities[Index], ScalarDensity))
            {
                ++Result.Mismatches;
                if (Result.FirstMismatch.IsEmpty())
                {
                    Result.FirstMismatch = FString::Printf(
                        TEXT("sample=(%d,%d,%d) block=%.9g scalar=%.9g delta=%.9g"),
                        SamplePos.X, SamplePos.Y, SamplePos.Z, Result.BlockDensities[Index],
                        ScalarDensity, Result.BlockDensities[Index] - ScalarDensity);
                }
            }
        }
        return Result;
    }

    static int32 FindGapChunkZ(const VoxelForgeTest::FTestWorld& World)
    {
        const int32 Min = World.BottomChunkZ - 8;
        const int32 Max = World.TopChunkZ + 8;
        for (int32 ChunkZ = Min; ChunkZ <= Max; ++ChunkZ)
        {
            if (World.StrateManager->IsGapChunk(FIntVector(0, 0, ChunkZ)))
            {
                return ChunkZ;
            }
        }
        return MIN_int32;
    }

    static int32 CountGapSamples(const VoxelForgeTest::FTestWorld& World,
                                 FIntVector Origin, int32 Step)
    {
        TSet<int32> SampleChunkZs;
        for (int32 Z = 0; Z < MesherGridSize; ++Z)
        {
            SampleChunkZs.Add(FMath::FloorToInt(
                static_cast<float>(Origin.Z + Z * Step) / CHUNK_SIZE));
        }
        int32 Count = 0;
        for (const int32 ChunkZ : SampleChunkZs)
        {
            if (World.StrateManager->IsGapChunk(FIntVector(0, 0, ChunkZ)))
            {
                ++Count;
            }
        }
        return Count;
    }

    struct FScopedConsoleIntOverride
    {
        IConsoleVariable* Variable = nullptr;
        int32 OriginalValue = 0;

        explicit FScopedConsoleIntOverride(const TCHAR* Name, int32 Value)
            : Variable(IConsoleManager::Get().FindConsoleVariable(Name))
        {
            if (Variable != nullptr)
            {
                OriginalValue = Variable->GetInt();
                Variable->Set(Value, ECVF_SetByCode);
            }
        }

        ~FScopedConsoleIntOverride()
        {
            if (Variable != nullptr)
            {
                Variable->Set(OriginalValue, ECVF_SetByCode);
            }
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeDensityBlockEquivalenceTest,
    "VoxelForge.Density.OperatorBlockMatchesScalarGrid",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeDensityBlockEquivalenceTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // The production mesher initializes these once at startup. This headless test does so too,
    // then temporarily disables the fused TunnelNetwork evaluator so the real per-chunk
    // BeginDensityBlock -> BuildActiveOpList -> EvalBlock path is compared against scalar output.
    VoxelGenLOD::InitializeConsoleSwitches();
    FScopedConsoleIntOverride FusedEvaluatorOverride(TEXT("voxel.UseFusedEvaluator"), 0);
    if (FusedEvaluatorOverride.Variable == nullptr)
    {
        AddError(TEXT("voxel.UseFusedEvaluator cvar is unavailable; block route was not forced"));
        return false;
    }

    static const int32 Seeds[] = {1337, 9176, 17015, 24854, 32693, 40532};
    static const ECaveGeneratorType Archetypes[ArchetypeCount] = {
        ECaveGeneratorType::TunnelNetwork,
        ECaveGeneratorType::FlatPlain,
        ECaveGeneratorType::CrystalChamber,
        ECaveGeneratorType::Maze,
        ECaveGeneratorType::SurfaceWorld,
        ECaveGeneratorType::VerticalShafts,
        ECaveGeneratorType::FloatingIslands,
        ECaveGeneratorType::Underwater,
    };

    int64 GeneratorSamples = 0;
    int32 GeneratorMismatches = 0;
    FString FirstGeneratorMismatch;
    int64 DirectStackSamples = 0;
    int32 DirectStackMismatches = 0;
    FString FirstDirectStackMismatch;
    int32 DirectStackBlocks = 0;
    int32 GeneratorBlocks = 0;
    int32 ProductionArchetypeBlocks = 0;
    int32 BoundaryBlocks = 0;
    int32 NearOriginBlocks = 0;
    int32 FarOriginBlocks = 0;
    int32 GapCoverageBlocks = 0;
    int32 PassageFocusedBlocks = 0;
    int32 SpecialGapCoverageBlocks = 0;
    int32 EditedSamplesChanged = 0;
    int32 AcceptedEdits = 0;
    int32 ArchetypeLevelCounts[ArchetypeCount][6] = {};

    for (int32 SeedIndex = 0; SeedIndex < UE_ARRAY_COUNT(Seeds); ++SeedIndex)
    {
        VoxelForgeTest::FTestWorld World;
        World.Build(Seeds[SeedIndex], /*gap chunks=*/2, /*operator stack=*/true);
        if (!World.IsValid())
        {
            AddError(World.WhyInvalid());
            return false;
        }
        // The fixture uses coarse-LOD edits up to 64 voxels wide. Keep the actual diff layer in
        // the route while allowing those bounded test brushes.
        World.DiffLayer->SetBudget(/*max mods=*/0, /*max radius=*/256.0f,
                                   /*max accumulated volume=*/0.0f);

        const TArray<FStrateSlot>& Slots = World.StrateManager->GetLayout();
        const TArray<FVoxelPassage>& Passages = World.StrateManager->GetPassages();
        if (Slots.Num() < ArchetypeCount || Passages.IsEmpty())
        {
            AddError(FString::Printf(
                TEXT("seed %d is missing an archetype slot or passage fixture"), Seeds[SeedIndex]));
            return false;
        }
        const int32 GapChunkZ = FindGapChunkZ(World);
        if (GapChunkZ == MIN_int32)
        {
            AddError(FString::Printf(TEXT("seed %d produced no gap chunk fixture"), Seeds[SeedIndex]));
            return false;
        }

        FIntVector GridOrigins[GridSetCount];
        int32 GridSteps[GridSetCount] = {};
        int32 GridArchetypes[GridSetCount] = {};
        FGridComparison BaseResults[GridSetCount];

        // One complete mesher lattice for each production archetype per seed. Rotating level by
        // seed gives every archetype all levels 0..5 over the six seeds. The sample grids touch a
        // strate edge, and cycle through origin, near-origin, and both far-field signs.
        for (int32 ArchetypeIndex = 0; ArchetypeIndex < ArchetypeCount; ++ArchetypeIndex)
        {
            const int32 Level = (SeedIndex + ArchetypeIndex) % 6;
            const int32 Step = 1 << Level;
            const FStrateSlot& Slot = Slots[ArchetypeIndex];
            const int32 TargetChunkZ = (SeedIndex & 1) ? Slot.BottomChunkZ : Slot.TopChunkZ;
            FIntVector TargetChunk;
            switch ((SeedIndex + ArchetypeIndex) % 4)
            {
            case 0: TargetChunk = FIntVector(0, 0, TargetChunkZ); break;
            case 1: TargetChunk = FIntVector(128, 96, TargetChunkZ); ++FarOriginBlocks; break;
            case 2: TargetChunk = FIntVector(8, -8, TargetChunkZ); ++NearOriginBlocks; break;
            default: TargetChunk = FIntVector(-128, -96, TargetChunkZ); ++FarOriginBlocks; break;
            }
            if ((SeedIndex + ArchetypeIndex) % 4 == 0) { ++NearOriginBlocks; }

            const int32 GridIndex = ArchetypeIndex;
            GridOrigins[GridIndex] = MakeMesherGridOrigin(TargetChunk, Level);
            GridSteps[GridIndex] = Step;
            GridArchetypes[GridIndex] = ArchetypeIndex;

            FVoxelOpStack Stack;
            FVoxelOpContext Context;
            if (!BuildProductionStackForArchetype(
                    World, Archetypes[ArchetypeIndex], TargetChunk, Stack, Context))
            {
                AddError(FString::Printf(
                    TEXT("seed %d archetype %d production stack could not be built"),
                    Seeds[SeedIndex], ArchetypeIndex));
                return false;
            }
            Context.Step = 1;
            Stack.PrepareChunk(Context);
            const FGridComparison DirectComparison = CompareStackChunkBlockWithScalar(
                Stack, GridOrigins[GridIndex], Step, TargetChunk);
            DirectStackSamples += DirectComparison.Samples;
            DirectStackMismatches += DirectComparison.Mismatches;
            ++DirectStackBlocks;
            if (FirstDirectStackMismatch.IsEmpty() && DirectComparison.Mismatches > 0)
            {
                FirstDirectStackMismatch = FString::Printf(
                    TEXT("seed=%d archetype=%d level=%d chunk=%s %s"),
                    Seeds[SeedIndex], ArchetypeIndex, Level, *TargetChunk.ToString(),
                    *DirectComparison.FirstMismatch);
            }

            ++ArchetypeLevelCounts[ArchetypeIndex][Level];
            ++ProductionArchetypeBlocks;
            ++BoundaryBlocks;

            if (!World.StrateManager->UsesOperatorStackForChunk(TargetChunk)
                || World.StrateManager->GetGeneratorTypeForChunk(TargetChunk)
                    != Archetypes[ArchetypeIndex])
            {
                AddError(FString::Printf(
                    TEXT("seed %d archetype %d target chunk did not route to its production operator stack"),
                    Seeds[SeedIndex], ArchetypeIndex));
                return false;
            }
        }

        // Passage-mouth lattices centered in a real inter-strate gap exercise the routed
        // TunnelNetwork/Underwater block implementation through boundaries and bedrock holes.
        for (int32 PassageGrid = 0; PassageGrid < PassageGridCount; ++PassageGrid)
        {
            const int32 GridIndex = ArchetypeCount + PassageGrid;
            const int32 ArchetypeIndex = PassageGrid == 0 ? 0 : 7;
            const int32 Level = (SeedIndex + (PassageGrid == 0 ? 0 : 7)) % 6;
            const int32 Step = 1 << Level;
            const FVoxelPassage& Passage = Passages[(SeedIndex + PassageGrid) % Passages.Num()];
            const FVector Focus = PassageGrid == 0 ? Passage.UpperPoint : Passage.LowerPoint;
            const FIntVector TargetChunk(
                FMath::FloorToInt(Focus.X / CHUNK_SIZE),
                FMath::FloorToInt(Focus.Y / CHUNK_SIZE),
                GapChunkZ);
            GridOrigins[GridIndex] = MakeMesherGridOrigin(TargetChunk, Level);
            GridSteps[GridIndex] = Step;
            GridArchetypes[GridIndex] = ArchetypeIndex;
            ++PassageFocusedBlocks;
            if (CountGapSamples(World, GridOrigins[GridIndex], Step) > 0)
            {
                ++GapCoverageBlocks;
                ++SpecialGapCoverageBlocks;
            }
        }

        // No-edit grids. In the two canonical network archetypes this enters the actual worker
        // block session, which partitions each mesher grid by resolved chunk before running the
        // active operator list. The six legacy density routes still run their production scalar
        // evaluator, as they do outside the block-safe network branch.
        for (int32 GridIndex = 0; GridIndex < GridSetCount; ++GridIndex)
        {
            BaseResults[GridIndex] = CompareGeneratorBlockWithScalar(
                *World.Generator.Get(), GridOrigins[GridIndex], GridSteps[GridIndex]);
            GeneratorSamples += BaseResults[GridIndex].Samples;
            GeneratorMismatches += BaseResults[GridIndex].Mismatches;
            ++GeneratorBlocks;
            if (FirstGeneratorMismatch.IsEmpty() && BaseResults[GridIndex].Mismatches > 0)
            {
                FirstGeneratorMismatch = FString::Printf(
                    TEXT("seed=%d archetype=%d level=%d edited=0 origin=%s %s"),
                    Seeds[SeedIndex], GridArchetypes[GridIndex],
                    FMath::FloorLog2(GridSteps[GridIndex]), *GridOrigins[GridIndex].ToString(),
                    *BaseResults[GridIndex].FirstMismatch);
            }
        }

        // Put one real diff-layer box edit in every mesher grid, then compare the same exact
        // density lattices again. The scalar pass below runs with these player edits present.
        for (int32 GridIndex = 0; GridIndex < GridSetCount; ++GridIndex)
        {
            const int32 Step = GridSteps[GridIndex];
            FVoxelModification Edit;
            Edit.Shape = EVoxelBrushShape::Box;
            Edit.Center = FVector(GridOrigins[GridIndex]) + FVector(17 * Step);
            Edit.BoxExtent = FVector(2.0f * Step);
            Edit.Falloff = 1.0f;
            Edit.Strength = -13.0f;
            const TArray<FIntVector> ChangedChunks = World.DiffLayer->ApplyModification(Edit);
            if (ChangedChunks.IsEmpty())
            {
                AddError(FString::Printf(
                    TEXT("seed %d edit for grid %d archetype %d was rejected"),
                    Seeds[SeedIndex], GridIndex, GridArchetypes[GridIndex]));
                return false;
            }
            ++AcceptedEdits;
        }

        for (int32 GridIndex = 0; GridIndex < GridSetCount; ++GridIndex)
        {
            const FGridComparison Edited = CompareGeneratorBlockWithScalar(
                *World.Generator.Get(), GridOrigins[GridIndex], GridSteps[GridIndex]);
            GeneratorSamples += Edited.Samples;
            GeneratorMismatches += Edited.Mismatches;
            ++GeneratorBlocks;
            if (FirstGeneratorMismatch.IsEmpty() && Edited.Mismatches > 0)
            {
                FirstGeneratorMismatch = FString::Printf(
                    TEXT("seed=%d archetype=%d level=%d edited=1 origin=%s %s"),
                    Seeds[SeedIndex], GridArchetypes[GridIndex],
                    FMath::FloorLog2(GridSteps[GridIndex]), *GridOrigins[GridIndex].ToString(),
                    *Edited.FirstMismatch);
            }

            const int32 SharedCount = FMath::Min(
                Edited.BlockDensities.Num(), BaseResults[GridIndex].BlockDensities.Num());
            for (int32 SampleIndex = 0; SampleIndex < SharedCount; ++SampleIndex)
            {
                if (!SameFloatBits(Edited.BlockDensities[SampleIndex],
                                   BaseResults[GridIndex].BlockDensities[SampleIndex]))
                {
                    ++EditedSamplesChanged;
                }
            }
        }
    }

    bool bEveryArchetypeCoveredEveryLevel = true;
    for (int32 ArchetypeIndex = 0; ArchetypeIndex < ArchetypeCount; ++ArchetypeIndex)
    {
        for (int32 Level = 0; Level < 6; ++Level)
        {
            bEveryArchetypeCoveredEveryLevel &= ArchetypeLevelCounts[ArchetypeIndex][Level] > 0;
        }
    }

    AddInfo(FString::Printf(
        TEXT("BeginDensityBlock grid vs scalar GetDensityAt: seeds=%d archetype_grids=%d all_archetype_levels=%d boundary_grids=%d near_origin=%d far_origin=%d blocks=%d samples=%lld mismatches=%d gap_grids=%d passage_focused=%d accepted_edits=%d edited_samples_changed=%d first=%s"),
        UE_ARRAY_COUNT(Seeds), ProductionArchetypeBlocks,
        bEveryArchetypeCoveredEveryLevel ? 1 : 0, BoundaryBlocks, NearOriginBlocks,
        FarOriginBlocks, GeneratorBlocks, static_cast<long long>(GeneratorSamples),
        GeneratorMismatches, GapCoverageBlocks, PassageFocusedBlocks, AcceptedEdits,
        EditedSamplesChanged,
        FirstGeneratorMismatch.IsEmpty() ? TEXT("none") : *FirstGeneratorMismatch));
    AddInfo(FString::Printf(
        TEXT("chunk-local direct EvalBlock vs EvalInternal: blocks=%d mesher_samples=%lld mismatches=%d first=%s"),
        DirectStackBlocks, static_cast<long long>(DirectStackSamples),
        DirectStackMismatches,
        FirstDirectStackMismatch.IsEmpty() ? TEXT("none") : *FirstDirectStackMismatch));

    TestEqual(TEXT("all eight production operator archetypes get six complete mesher grids"),
        ProductionArchetypeBlocks,
        static_cast<int32>(UE_ARRAY_COUNT(Seeds)) * ArchetypeCount);
    TestTrue(TEXT("every production operator archetype covers every meshing level 0..5"),
        bEveryArchetypeCoveredEveryLevel);
    TestEqual(TEXT("all production archetype grids touch a strate boundary"),
        BoundaryBlocks, ProductionArchetypeBlocks);
    TestTrue(TEXT("test includes near-origin and both-sign far-field grids"),
        NearOriginBlocks > 0 && FarOriginBlocks > 0);
    TestEqual(TEXT("each passage-focused grid includes a real gap sample"),
        SpecialGapCoverageBlocks,
        static_cast<int32>(UE_ARRAY_COUNT(Seeds)) * PassageGridCount);
    TestEqual(TEXT("passage-focused grids cover both network archetypes per seed"),
        PassageFocusedBlocks,
        static_cast<int32>(UE_ARRAY_COUNT(Seeds)) * PassageGridCount);
    TestEqual(TEXT("all block-session density grids match scalar output bit-for-bit"),
        GeneratorMismatches, 0);
    TestEqual(TEXT("all production stack chunk blocks match scalar bit-for-bit"),
        DirectStackMismatches, 0);
    TestEqual(TEXT("one valid player edit was applied to every tested grid"),
        AcceptedEdits, static_cast<int32>(UE_ARRAY_COUNT(Seeds)) * GridSetCount);
    TestTrue(TEXT("player edits alter samples consumed by the mesher"),
        EditedSamplesChanged > 0);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
