// The tile-reach proof must fail loudly when its envelope is deliberately made too small.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelMarchingCubesMesher.h"
#include "../../Public/VoxelTilePostReach.h"

namespace
{
    using namespace VoxelForgeTest;

    struct FScopedReachTestCVars
    {
        IConsoleVariable* Debug = nullptr;
        IConsoleVariable* Scale = nullptr;
        int32 OldDebug = 0;
        float OldScale = 1.0f;
        bool bValid = false;

        FScopedReachTestCVars()
        {
            IConsoleManager& Console = IConsoleManager::Get();
            Debug = Console.FindConsoleVariable(TEXT("voxel.TilePostReachDebug"));
            Scale = Console.FindConsoleVariable(TEXT("voxel.TilePostReachScale"));
            if (Debug == nullptr || Scale == nullptr)
            {
                return;
            }

            OldDebug = Debug->GetInt();
            OldScale = Scale->GetFloat();
            bValid = true;
            Debug->Set(1, ECVF_SetByCode);
        }

        ~FScopedReachTestCVars()
        {
            if (!bValid)
            {
                return;
            }
            Debug->Set(OldDebug, ECVF_SetByCode);
            Scale->Set(OldScale, ECVF_SetByCode);
        }

        void SetScale(float InScale)
        {
            if (Scale != nullptr)
            {
                Scale->Set(InScale, ECVF_SetByCode);
            }
        }
    };

    struct FReachSnapshot
    {
        uint64 Tiles = 0;
        uint64 FinalComparisons = 0;
        uint64 FinalDifferences = 0;
        uint64 AllComparisons = 0;
        uint64 AllDifferences = 0;
        int64 DensitySamples = 0;
        int32 MeshedTiles = 0;
    };

    FReachSnapshot RunReachTiles(
        UVoxelMarchingCubesMesher& Mesher,
        const TArray<FIntVector>& Origins)
    {
        FReachSnapshot Out;
        const uint64 TileCountBefore =
            VoxelGenLOD::GTilePostReachTileCount.load(std::memory_order_relaxed);
        for (const FIntVector& Origin : Origins)
        {
            int64 Samples = 0;
            Mesher.GenerateMesh(
                Origin, 8, 16, nullptr, INT32_MIN, INT32_MAX, &Samples);
            Out.DensitySamples += Samples;
            Out.MeshedTiles += Samples > 0 ? 1 : 0;
        }

        const uint64 TileCountAfter =
            VoxelGenLOD::GTilePostReachTileCount.load(std::memory_order_relaxed);
        Out.Tiles = TileCountAfter - TileCountBefore;
        Out.FinalComparisons = VoxelGenLOD::GSkippedPostComparisonsByKind[
            static_cast<uint8>(VoxelGenLOD::ETilePostComparisonKind::FinalField)]
            .load(std::memory_order_relaxed);
        Out.FinalDifferences = VoxelGenLOD::GSkippedPostDifferencesByKind[
            static_cast<uint8>(VoxelGenLOD::ETilePostComparisonKind::FinalField)]
            .load(std::memory_order_relaxed);
        Out.AllComparisons = VoxelGenLOD::GSkippedPostComparisons.load(
            std::memory_order_relaxed);
        Out.AllDifferences = VoxelGenLOD::GSkippedPostDifferences.load(
            std::memory_order_relaxed);
        return Out;
    }

    int32 FloorAlign(int32 Value, int32 Alignment)
    {
        return FMath::FloorToInt(static_cast<float>(Value) / static_cast<float>(Alignment))
            * Alignment;
    }

    void BuildReachTileOrigins(
        const FTestWorld& World,
        int32 Slot,
        TArray<FIntVector>& OutOrigins)
    {
        int32 SlotTop = 0;
        int32 SlotBottom = 0;
        if (!World.GetSlotVoxelZRange(Slot, SlotTop, SlotBottom))
        {
            return;
        }
        const int32 TileExtent = 16 * 8;
        const int32 TileZ = FloorAlign(SlotBottom, TileExtent);
        // Nine XY tiles around the origin cover the guaranteed spine, winding tunnel mouths,
        // and feature-free graph regions. Include one distant tile to prove the skip side is
        // exercised rather than relying only on near-tunnel tiles.
        for (int32 Y = -TileExtent; Y <= TileExtent; Y += TileExtent)
        {
            for (int32 X = -TileExtent; X <= TileExtent; X += TileExtent)
            {
                OutOrigins.Add(FIntVector(X, Y, TileZ));
            }
        }
        OutOrigins.Add(FIntVector(4 * TileExtent, -3 * TileExtent, TileZ));
    }

    void AppendWorldReachTiles(
        const FTestWorld& World,
        TArray<FIntVector>& OutOrigins)
    {
        BuildReachTileOrigins(World, FTestWorld::SlotTunnelNetwork, OutOrigins);
        BuildReachTileOrigins(World, FTestWorld::SlotUnderwater, OutOrigins);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeTilePostReachProofTest,
    "VoxelForge.Correctness.TilePostReachProof",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeTilePostReachProofTest::RunTest(const FString& Parameters)
{
    FScopedReachTestCVars CVarGuard;
    if (!CVarGuard.bValid)
    {
        AddError(TEXT("Tile-post reach test could not find its two console variables."));
        return false;
    }

    FTestWorld NativeWorld;
    NativeWorld.Build(/*Seed*/1337, /*GapChunks*/2, /*bUseOperatorStack*/false);
    FTestWorld FusedWorld;
    FusedWorld.Build(/*Seed*/1337, /*GapChunks*/2, /*bUseOperatorStack*/true);
    if (!NativeWorld.IsValid() || !FusedWorld.IsValid())
    {
        AddError(NativeWorld.IsValid() && FusedWorld.IsValid()
            ? TEXT("Unexpected invalid reach-proof fixture")
            : (!NativeWorld.IsValid() ? NativeWorld.WhyInvalid() : FusedWorld.WhyInvalid()));
        return false;
    }

    TArray<FIntVector> NativeOrigins;
    TArray<FIntVector> FusedOrigins;
    AppendWorldReachTiles(NativeWorld, NativeOrigins);
    AppendWorldReachTiles(FusedWorld, FusedOrigins);
    TestTrue(TEXT("the representative native tile set is non-empty"), NativeOrigins.Num() > 0);
    TestTrue(TEXT("the representative fused tile set is non-empty"), FusedOrigins.Num() > 0);

    TStrongObjectPtr<UVoxelMarchingCubesMesher> NativeMesher(
        NewObject<UVoxelMarchingCubesMesher>(GetTransientPackage(), NAME_None, RF_Transient));
    TStrongObjectPtr<UVoxelMarchingCubesMesher> FusedMesher(
        NewObject<UVoxelMarchingCubesMesher>(GetTransientPackage(), NAME_None, RF_Transient));
    NativeMesher->SetGenerator(NativeWorld.Generator.Get());
    FusedMesher->SetGenerator(FusedWorld.Generator.Get());

    VoxelGenLOD::ResetTilePostReachDiagnostics();
    CVarGuard.SetScale(1.0f);
    const FReachSnapshot NativeFull = RunReachTiles(*NativeMesher, NativeOrigins);
    const FReachSnapshot FusedFull = RunReachTiles(*FusedMesher, FusedOrigins);
    const uint64 FullFinalComparisons = NativeFull.FinalComparisons + FusedFull.FinalComparisons;
    const uint64 FullFinalDifferences = NativeFull.FinalDifferences + FusedFull.FinalDifferences;
    TestTrue(TEXT("normal reach compares final fields on the representative tiles"),
             FullFinalComparisons > 0);
    TestEqual(TEXT("normal reach has no final-field differences"), FullFinalDifferences, 0ull);
    TestEqual(TEXT("normal reach has no skipped-post differences"),
              NativeFull.AllDifferences + FusedFull.AllDifferences, 0ull);

    VoxelGenLOD::ResetTilePostReachDiagnostics();
    // Use a deliberately tiny but valid scale so at least one envelope is rejected while the same
    // final-field samples remain exercised; this is the negative control, not a supported setting.
    CVarGuard.SetScale(0.01f);
    const FReachSnapshot NativeShrunk = RunReachTiles(*NativeMesher, NativeOrigins);
    const FReachSnapshot FusedShrunk = RunReachTiles(*FusedMesher, FusedOrigins);
    const uint64 ShrunkFinalComparisons =
        NativeShrunk.FinalComparisons + FusedShrunk.FinalComparisons;
    const uint64 ShrunkFinalDifferences =
        NativeShrunk.FinalDifferences + FusedShrunk.FinalDifferences;
    TestTrue(TEXT("shrunk reach still executes final-field comparisons"),
             ShrunkFinalComparisons > 0);
    TestTrue(TEXT("shrunk reach demonstrably fails the final-field proof"),
             ShrunkFinalDifferences > 0);

    AddInfo(FString::Printf(
        TEXT("tile_post_reach_proof: native_tiles=%llu fused_tiles=%llu full_final=%llu/%llu "
             "shrunk_final=%llu/%llu native_samples=%lld/%lld fused_samples=%lld/%lld."),
        static_cast<unsigned long long>(NativeFull.Tiles),
        static_cast<unsigned long long>(FusedFull.Tiles),
        static_cast<unsigned long long>(FullFinalComparisons),
        static_cast<unsigned long long>(FullFinalDifferences),
        static_cast<unsigned long long>(ShrunkFinalComparisons),
        static_cast<unsigned long long>(ShrunkFinalDifferences),
        static_cast<long long>(NativeFull.DensitySamples),
        static_cast<long long>(NativeShrunk.DensitySamples),
        static_cast<long long>(FusedFull.DensitySamples),
        static_cast<long long>(FusedShrunk.DensitySamples)));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
