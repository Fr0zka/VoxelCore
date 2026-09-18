// VoxelForgeOpStackChannelTest.cpp
// The channel declarations are executable stack-assembly metadata, not comments.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeOpStackChannelTest,
    "VoxelForge.OpStack.ChannelDAG",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    /** A deliberately small declaration-only op for the validator's negative controls. */
    class FDeclarationProbeOp final : public IVoxelDensityOp
    {
    public:
        FDeclarationProbeOp(EVoxelOpChannelMask InReads,
                            EVoxelOpChannelMask InWrites,
                            bool bInAdditive)
            : Reads(InReads), Writes(InWrites), bAdditive(bInAdditive)
        {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override { return Reads; }
        EVoxelOpChannelMask ChannelWrites() const override { return Writes; }
        bool IsAdditive() const override { return bAdditive; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        void Eval(float, float, float, FVoxelOpSample&) const override {}
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return EVoxelOpEffect::Identity;
        }

        const TCHAR* DebugName() const override { return TEXT("DeclarationProbeOp"); }

    private:
        EVoxelOpChannelMask Reads;
        EVoxelOpChannelMask Writes;
        bool bAdditive;
    };
}

bool FVoxelForgeOpStackChannelTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    FTestWorld World;
    World.Build();
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }

    const UVoxelGenerator* Generator = World.Generator.Get();
    const int32 Seed = World.Settings->Seed;

    auto MidChunkForSlot = [&](int32 Slot, const TCHAR* Name, int32& OutMidChunkZ) -> bool
    {
        int32 TopVoxelZ = 0;
        int32 BottomVoxelZ = 0;
        if (!World.GetSlotVoxelZRange(Slot, TopVoxelZ, BottomVoxelZ))
        {
            AddError(FString::Printf(
                TEXT("The fixture has no %s slot; check FTestWorld::Build's Archetypes[] and "
                     "the corresponding slot constant."), Name));
            return false;
        }
        OutMidChunkZ = ((TopVoxelZ + BottomVoxelZ) / 2) / CHUNK_SIZE;
        return true;
    };

    auto Validate = [&](FVoxelOpStack& Stack, const TCHAR* Name, int32 ExpectedNum)
    {
        TestEqual(*FString::Printf(TEXT("%s shipping stack op count"), Name),
                  Stack.Num(), ExpectedNum);

        FString Error;
        const bool bValid = Stack.ValidateChannelOrder(&Error);
        if (!bValid)
        {
            AddError(FString::Printf(TEXT("%s shipping stack failed channel validation: %s"),
                                     Name, *Error));
        }
        TestTrue(*FString::Printf(TEXT("%s shipping stack has a legal channel DAG"), Name), bValid);
    };

    // These are the eight builders currently shipped behind bUseOperatorStack. Keep the
    // archetype list explicit so adding a builder without adding its validator assertion is
    // visible in this test.
    int32 MidChunkZ = 0;

    if (MidChunkForSlot(FTestWorld::SlotTunnelNetwork, TEXT("TunnelNetwork"), MidChunkZ))
    {
        const FStrateGenerationParams Params =
            World.StrateManager->GetGenerationParams(FIntVector(0, 0, MidChunkZ));
        FVoxelOpStack Stack;
        VoxelDensityOps::BuildTunnelNetworkStack(
            Stack, Params, Seed, Generator->OriginSpineRadius, World.StrateManager.Get());
        Validate(Stack, TEXT("TunnelNetwork"), 20);
    }

    if (MidChunkForSlot(FTestWorld::SlotFlatPlain, TEXT("FlatPlain"), MidChunkZ))
    {
        const FSlabGenerationParams Params =
            World.StrateManager->GetSlabParamsForChunk(FIntVector(0, 0, MidChunkZ));
        FVoxelOpStack Stack;
        VoxelDensityOps::BuildSlabStack(
            Stack, Params, Seed, Generator->OriginSpineRadius, World.StrateManager.Get());
        Validate(Stack, TEXT("FlatPlain"), 6);
    }

    if (MidChunkForSlot(FTestWorld::SlotCrystalChamber, TEXT("CrystalChamber"), MidChunkZ))
    {
        const FSlabGenerationParams Params =
            World.StrateManager->GetSlabParamsForChunk(FIntVector(0, 0, MidChunkZ));
        FVoxelOpStack Stack;
        VoxelDensityOps::BuildSlabStack(
            Stack, Params, Seed, Generator->OriginSpineRadius, World.StrateManager.Get());
        Validate(Stack, TEXT("CrystalChamber"), 6);
    }

    if (MidChunkForSlot(FTestWorld::SlotMaze, TEXT("Maze"), MidChunkZ))
    {
        const FMazeGenerationParams Params =
            World.StrateManager->GetMazeParamsForChunk(FIntVector(0, 0, MidChunkZ));
        FVoxelOpStack Stack;
        VoxelDensityOps::BuildMazeStack(
            Stack, Params, Seed, Generator->OriginSpineRadius, World.StrateManager.Get());
        Validate(Stack, TEXT("Maze"), 8);
    }

    if (MidChunkForSlot(FTestWorld::SlotSurfaceWorld, TEXT("SurfaceWorld"), MidChunkZ))
    {
        const FSurfaceGenerationParams Params =
            World.StrateManager->GetSurfaceParamsForChunk(FIntVector(0, 0, MidChunkZ));
        FVoxelOpStack Stack;
        VoxelDensityOps::BuildSurfaceStack(
            Stack, Params, Seed, Generator->OriginSpineRadius, World.StrateManager.Get());
        Validate(Stack, TEXT("SurfaceWorld"), 6);
    }

    if (MidChunkForSlot(FTestWorld::SlotVerticalShafts, TEXT("VerticalShafts"), MidChunkZ))
    {
        const FVerticalShaftParams Params =
            World.StrateManager->GetVerticalShaftParamsForChunk(FIntVector(0, 0, MidChunkZ));
        FVoxelOpStack Stack;
        VoxelDensityOps::BuildVerticalShaftStack(
            Stack, Params, Seed, Generator->OriginSpineRadius, World.StrateManager.Get());
        Validate(Stack, TEXT("VerticalShafts"), 11);
    }

    if (MidChunkForSlot(FTestWorld::SlotFloatingIsland, TEXT("FloatingIslands"), MidChunkZ))
    {
        const FFloatingIslandParams Params =
            World.StrateManager->GetFloatingIslandParamsForChunk(FIntVector(0, 0, MidChunkZ));
        FVoxelOpStack Stack;
        VoxelDensityOps::BuildFloatingIslandStack(
            Stack, Params, Seed, Generator->OriginSpineRadius, World.StrateManager.Get());
        Validate(Stack, TEXT("FloatingIslands"), 8);
    }

    if (MidChunkForSlot(FTestWorld::SlotUnderwater, TEXT("Underwater"), MidChunkZ))
    {
        const FStrateGenerationParams Params =
            World.StrateManager->GetGenerationParams(FIntVector(0, 0, MidChunkZ));
        FVoxelOpStack Stack;
        VoxelDensityOps::BuildTunnelNetworkStack(
            Stack, Params, Seed, Generator->OriginSpineRadius, World.StrateManager.Get());
        Validate(Stack, TEXT("Underwater"), 20);
    }

    // A declaration that reads an unavailable channel must fail, and a second write-only
    // replacement must not silently erase the first producer. These controls pin the validator's
    // two most important safety rules independently of the shipping builders.
    {
        FVoxelOpStack MissingProducer;
        MissingProducer.Add(MakeUnique<FDeclarationProbeOp>(
            VoxelOpChannels::Density, VoxelOpChannels::Density, true));
        FString Error;
        TestFalse(TEXT("a read before its first producer is rejected"),
                  MissingProducer.ValidateChannelOrder(&Error));
    }

    {
        FVoxelOpStack Clobber;
        Clobber.Add(MakeUnique<FDeclarationProbeOp>(
            VoxelOpChannels::None, VoxelOpChannels::Density, false));
        Clobber.Add(MakeUnique<FDeclarationProbeOp>(
            VoxelOpChannels::None, VoxelOpChannels::Density, false));
        FString Error;
        TestFalse(TEXT("a second write-only replacement is rejected"),
                  Clobber.ValidateChannelOrder(&Error));
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
