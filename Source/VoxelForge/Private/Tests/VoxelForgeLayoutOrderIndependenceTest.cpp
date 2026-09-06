// VoxelForgeLayoutOrderIndependenceTest.cpp
// Test de déterminisme du layout et des passages quand l'ordre du pool change.
// Determinism test for the layout and passages when the pool order changes.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelForgeTestFixture.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeLayoutOrderIndependenceTest,
    "VoxelForge.Determinism.LayoutOrderIndependence",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    constexpr int32 LayoutSeed = 20260829;
    constexpr int32 NumPoolDefinitions = 5;

    struct FLayoutSnapshot
    {
        TArray<FStrateSlot> Layout;
        TArray<FVoxelPassage> Passages;
    };

    /**
     * Small headless world whose pool has no FixedStrates entries. That is important: the test
     * must exercise the shuffled pool path rather than accidentally testing only pinned slots.
     *
     * Petit monde sans entrée FixedStrates. C'est essentiel : le test doit exercer le chemin du
     * pool mélangé, et ne pas tester par accident uniquement des slots épinglés.
     */
    struct FLayoutTestWorld
    {
        TStrongObjectPtr<UVoxelSettings> Settings;
        TStrongObjectPtr<UVoxelStrateManager> StrateManager;
        TArray<TStrongObjectPtr<UVoxelStrateDefinition>> Definitions;

        void Build()
        {
            Settings = TStrongObjectPtr<UVoxelSettings>(
                NewObject<UVoxelSettings>(GetTransientPackage(), NAME_None, RF_Transient));
            Settings->Seed = LayoutSeed;
            Settings->TotalStrates = NumPoolDefinitions;
            Settings->InterStrateGapChunks = 1;
            Settings->bOpenSurfaceEntry = true;

            static const ECaveGeneratorType GeneratorTypes[] = {
                ECaveGeneratorType::TunnelNetwork,
                ECaveGeneratorType::FlatPlain,
                ECaveGeneratorType::Maze,
                ECaveGeneratorType::SurfaceWorld,
                ECaveGeneratorType::VerticalShafts,
            };
            static const EVoxelPassageStyle PassageStyles[] = {
                EVoxelPassageStyle::Straight,
                EVoxelPassageStyle::Worm,
                EVoxelPassageStyle::Spiral,
                EVoxelPassageStyle::Cascading,
                EVoxelPassageStyle::Worm,
            };

            for (int32 Index = 0; Index < NumPoolDefinitions; ++Index)
            {
                UVoxelStrateDefinition* Def = NewObject<UVoxelStrateDefinition>(
                    GetTransientPackage(), NAME_None, RF_Transient);
                Def->StrateName = FText::FromString(
                    FString::Printf(TEXT("Layout order test strate %d"), Index));
                Def->GeneratorType = GeneratorTypes[Index];
                Def->StrateHeightInChunks = 3 + Index;
                Def->TransitionType = EVoxelStrateTransition::Hard;

                // Non-trivial, distinct passage configs exercise the connection index and the
                // shape seeds while keeping every generated passage observable in the snapshot.
                // Des configs distinctes rendent visibles l'index de connexion et les seeds de
                // forme, tout en gardant chaque passage généré observable dans le snapshot.
                FStratePassageConfig& Passage = Def->PassageConfig;
                Passage.Connections = Index + 1;
                Passage.Style = PassageStyles[Index];
                Passage.MouthRadius = 5.0f + (float)Index;
                Passage.MidRadius = 3.0f + (float)Index * 0.5f;
                Passage.ReachMin = 12.0f + (float)Index * 3.0f;
                Passage.ReachMax = 48.0f + (float)Index * 4.0f;
                Passage.DistanceMin = 20.0f + (float)Index * 5.0f;
                Passage.DistanceMax = 80.0f + (float)Index * 7.0f;
                Passage.Wander = 20.0f + (float)Index;
                Passage.Segments = 4 + Index;
                Passage.VerticalWobble = 4.0f;
                Passage.SpiralRadius = 10.0f + (float)Index;
                Passage.SpiralTurns = 1.0f + (float)Index * 0.25f;
                Passage.CascadeSteps = 2 + Index;
                Passage.CascadeLedge = 10.0f + (float)Index;

                Definitions.Add(TStrongObjectPtr<UVoxelStrateDefinition>(Def));
            }

            StrateManager = TStrongObjectPtr<UVoxelStrateManager>(
                NewObject<UVoxelStrateManager>(GetTransientPackage(), NAME_None, RF_Transient));
        }

        void SetPoolOrder(const TArray<int32>& Order)
        {
            Settings->StratePool.Reset(Order.Num());
            for (const int32 DefinitionIndex : Order)
            {
                Settings->StratePool.Add(
                    TSoftObjectPtr<UVoxelStrateDefinition>(Definitions[DefinitionIndex].Get()));
            }
        }

        bool Rebuild(const TArray<int32>& Order)
        {
            if (!Settings.IsValid() || !StrateManager.IsValid())
            {
                return false;
            }
            SetPoolOrder(Order);
            StrateManager->Initialize(Settings.Get(), LayoutSeed);
            return true;
        }
    };

    FLayoutSnapshot CaptureSnapshot(const UVoxelStrateManager& Manager)
    {
        FLayoutSnapshot Snapshot;
        Snapshot.Layout = Manager.GetLayout();
        Snapshot.Passages = Manager.GetPassages();
        return Snapshot;
    }

    bool SameVectorBits(const FVector& A, const FVector& B)
    {
        return VoxelForgeTest::BitEqual(A.X, B.X)
            && VoxelForgeTest::BitEqual(A.Y, B.Y)
            && VoxelForgeTest::BitEqual(A.Z, B.Z);
    }

    bool SameLanding(const FVoxelPassageLanding& A, const FVoxelPassageLanding& B)
    {
        return SameVectorBits(A.StandingPoint, B.StandingPoint)
            && SameVectorBits(A.DoorPoint, B.DoorPoint)
            && SameVectorBits(A.DoorDirection, B.DoorDirection)
            && SameVectorBits(A.ConnectorStart, B.ConnectorStart)
            && SameVectorBits(A.ConnectorControl, B.ConnectorControl)
            && SameVectorBits(A.ConnectorEnd, B.ConnectorEnd)
            && VoxelForgeTest::BitEqual(A.FloorZ, B.FloorZ)
            && VoxelForgeTest::BitEqual(A.CeilingZ, B.CeilingZ)
            && VoxelForgeTest::BitEqual(A.HalfWidth, B.HalfWidth)
            && VoxelForgeTest::BitEqual(A.FloorThickness, B.FloorThickness)
            && VoxelForgeTest::BitEqual(A.ConnectorRadius, B.ConnectorRadius)
            && VoxelForgeTest::BitEqual(A.ConnectorCeilingZ, B.ConnectorCeilingZ)
            && VoxelForgeTest::BitEqual(A.RootFloorZ, B.RootFloorZ)
            && VoxelForgeTest::BitEqual(A.RootCeilingZ, B.RootCeilingZ)
            && VoxelForgeTest::BitEqual(A.RootSpineRadius, B.RootSpineRadius)
            && A.bSourcePlayerFit == B.bSourcePlayerFit
            && A.bHasNetworkConnector == B.bHasNetworkConnector
            && A.bHasConnectorBend == B.bHasConnectorBend;
    }

    bool SamePassage(const FVoxelPassage& A, const FVoxelPassage& B, FString& OutMismatch,
                     int32 PassageIndex)
    {
        if (A.UpperStrateIndex != B.UpperStrateIndex || A.LowerStrateIndex != B.LowerStrateIndex)
        {
            OutMismatch = FString::Printf(
                TEXT("passage %d strate indices differ (%d/%d vs %d/%d)"), PassageIndex,
                A.UpperStrateIndex, A.LowerStrateIndex, B.UpperStrateIndex, B.LowerStrateIndex);
            return false;
        }

        if (!SameVectorBits(A.UpperPoint, B.UpperPoint)
            || !SameVectorBits(A.LowerPoint, B.LowerPoint))
        {
            OutMismatch = FString::Printf(
                TEXT("passage %d endpoints differ"), PassageIndex);
            return false;
        }

        if (!SameLanding(A.UpperLanding, B.UpperLanding)
            || !SameLanding(A.LowerLanding, B.LowerLanding))
        {
            OutMismatch = FString::Printf(
                TEXT("passage %d landing descriptors differ"), PassageIndex);
            return false;
        }

        if (!VoxelForgeTest::BitEqual(A.Radius, B.Radius))
        {
            OutMismatch = FString::Printf(
                TEXT("passage %d radius differs (%.9g vs %.9g)"), PassageIndex, A.Radius, B.Radius);
            return false;
        }

        if (A.PassageType != B.PassageType)
        {
            OutMismatch = FString::Printf(
                TEXT("passage %d type differs (%d vs %d)"), PassageIndex,
                (int32)A.PassageType, (int32)B.PassageType);
            return false;
        }

        if (A.ControlPoints.Num() != B.ControlPoints.Num())
        {
            OutMismatch = FString::Printf(
                TEXT("passage %d control-point count differs (%d vs %d)"), PassageIndex,
                A.ControlPoints.Num(), B.ControlPoints.Num());
            return false;
        }
        for (int32 PointIndex = 0; PointIndex < A.ControlPoints.Num(); ++PointIndex)
        {
            if (!SameVectorBits(A.ControlPoints[PointIndex], B.ControlPoints[PointIndex]))
            {
                OutMismatch = FString::Printf(
                    TEXT("passage %d control point %d differs"), PassageIndex, PointIndex);
                return false;
            }
        }

        if (A.ControlRadii.Num() != B.ControlRadii.Num())
        {
            OutMismatch = FString::Printf(
                TEXT("passage %d control-radius count differs (%d vs %d)"), PassageIndex,
                A.ControlRadii.Num(), B.ControlRadii.Num());
            return false;
        }
        for (int32 RadiusIndex = 0; RadiusIndex < A.ControlRadii.Num(); ++RadiusIndex)
        {
            if (!VoxelForgeTest::BitEqual(A.ControlRadii[RadiusIndex], B.ControlRadii[RadiusIndex]))
            {
                OutMismatch = FString::Printf(
                    TEXT("passage %d control radius %d differs"), PassageIndex, RadiusIndex);
                return false;
            }
        }

        if (!SameVectorBits(A.BoundCenter, B.BoundCenter)
            || !VoxelForgeTest::BitEqual(A.BoundRadius, B.BoundRadius)
            || !VoxelForgeTest::BitEqual(A.BoundRadiusSq, B.BoundRadiusSq))
        {
            OutMismatch = FString::Printf(
                TEXT("passage %d bounding sphere differs"), PassageIndex);
            return false;
        }

        return true;
    }

    bool SameLayout(const TArray<FStrateSlot>& Expected, const TArray<FStrateSlot>& Actual,
                    FString& OutMismatch)
    {
        if (Expected.Num() != Actual.Num())
        {
            OutMismatch = FString::Printf(
                TEXT("layout slot count differs (%d vs %d)"), Expected.Num(), Actual.Num());
            return false;
        }

        for (int32 SlotIndex = 0; SlotIndex < Expected.Num(); ++SlotIndex)
        {
            const FStrateSlot& A = Expected[SlotIndex];
            const FStrateSlot& B = Actual[SlotIndex];
            if (A.Definition != B.Definition || A.StrateIndex != B.StrateIndex)
            {
                OutMismatch = FString::Printf(
                    TEXT("slot %d definition or strate index differs"), SlotIndex);
                return false;
            }
            if (A.TopChunkZ != B.TopChunkZ || A.BottomChunkZ != B.BottomChunkZ
                || A.HeightInChunks != B.HeightInChunks)
            {
                OutMismatch = FString::Printf(
                    TEXT("slot %d Z/height differs (top/bottom/height %d/%d/%d vs %d/%d/%d)"),
                    SlotIndex, A.TopChunkZ, A.BottomChunkZ, A.HeightInChunks,
                    B.TopChunkZ, B.BottomChunkZ, B.HeightInChunks);
                return false;
            }
        }

        return true;
    }

    bool SameSnapshot(const FLayoutSnapshot& Expected, const FLayoutSnapshot& Actual,
                      FString& OutMismatch)
    {
        if (!SameLayout(Expected.Layout, Actual.Layout, OutMismatch))
        {
            return false;
        }

        if (Expected.Passages.Num() != Actual.Passages.Num())
        {
            OutMismatch = FString::Printf(
                TEXT("passage count differs (%d vs %d)"),
                Expected.Passages.Num(), Actual.Passages.Num());
            return false;
        }

        for (int32 PassageIndex = 0; PassageIndex < Expected.Passages.Num(); ++PassageIndex)
        {
            if (!SamePassage(Expected.Passages[PassageIndex], Actual.Passages[PassageIndex],
                             OutMismatch, PassageIndex))
            {
                return false;
            }
        }

        return true;
    }
}

bool FVoxelForgeLayoutOrderIndependenceTest::RunTest(const FString& Parameters)
{
    FLayoutTestWorld World;
    World.Build();

    TArray<int32> OriginalOrder;
    for (int32 Index = 0; Index < NumPoolDefinitions; ++Index)
    {
        OriginalOrder.Add(Index);
    }

    TArray<int32> ReversedOrder;
    for (int32 Index = NumPoolDefinitions - 1; Index >= 0; --Index)
    {
        ReversedOrder.Add(Index);
    }

    TArray<int32> PermutedOrder = OriginalOrder;
    PermutedOrder.Swap(0, 2);

    TArray<TArray<int32>> Orders;
    Orders.Add(OriginalOrder);
    Orders.Add(ReversedOrder);
    Orders.Add(PermutedOrder);

    static const TCHAR* OrderNames[] = {
        TEXT("original"),
        TEXT("reversal"),
        TEXT("swap"),
    };

    TArray<FLayoutSnapshot> Snapshots;
    Snapshots.Reserve(Orders.Num());
    for (int32 OrderIndex = 0; OrderIndex < Orders.Num(); ++OrderIndex)
    {
        const bool bRebuilt = World.Rebuild(Orders[OrderIndex]);
        const int32 NumStrates = World.StrateManager.IsValid()
            ? World.StrateManager->GetNumStrates()
            : 0;
        if (!bRebuilt || NumStrates != NumPoolDefinitions)
        {
            AddError(FString::Printf(
                TEXT("Permutation '%s' could not build a strate layout at all. The test cannot ")
                TEXT("silently pass with an incomplete layout; expected %d resolved slots, got %d. ")
                TEXT("Check transient soft-pointer resolution."),
                OrderNames[OrderIndex], NumPoolDefinitions, NumStrates));
            return false;
        }

        Snapshots.Add(CaptureSnapshot(*World.StrateManager.Get()));
    }

    if (Snapshots.Num() == 0 || Snapshots[0].Layout.Num() == 0)
    {
        AddError(TEXT("No layout snapshot was inspected. This test would otherwise pass without ")
                 TEXT("checking any slots."));
        return false;
    }
    if (Snapshots[0].Passages.Num() == 0)
    {
        AddError(TEXT("The known pool generated no passages, so the passage invariant was not ")
                 TEXT("actually inspected."));
        return false;
    }

    int32 NumSlotsInspected = 0;
    int32 NumPassagesInspected = 0;
    for (int32 SnapshotIndex = 1; SnapshotIndex < Snapshots.Num(); ++SnapshotIndex)
    {
        NumSlotsInspected += Snapshots[0].Layout.Num();
        NumPassagesInspected += Snapshots[0].Passages.Num();

        FString Mismatch;
        if (!SameSnapshot(Snapshots[0], Snapshots[SnapshotIndex], Mismatch))
        {
            AddError(FString::Printf(
                TEXT("Pool order changed the built result for permutation '%s': %s"),
                OrderNames[SnapshotIndex], *Mismatch));
        }
    }

    // A pool permutation alone cannot expose the old sequential passage stream once the layout
    // itself is stable. Perturb one early connection count and verify that every later boundary
    // keeps its own (strate index, connection index) values unchanged.
    // Une permutation seule ne peut plus révéler l'ancien stream séquentiel une fois le layout
    // stable. On perturbe donc une connexion précoce et on vérifie toutes les frontières suivantes.
    UVoxelStrateDefinition* EarlyDefinition = Snapshots[0].Layout[0].Definition;
    if (!EarlyDefinition)
    {
        AddError(TEXT("The reference layout has a null definition in slot 0; no passage ")
                 TEXT("independence check can be performed."));
        return false;
    }

    const int32 OriginalConnections = EarlyDefinition->PassageConfig.Connections;
    EarlyDefinition->PassageConfig.Connections = OriginalConnections + 1;
    const bool bPerturbedRebuild = World.Rebuild(OriginalOrder);
    const int32 NumPerturbedStrates = World.StrateManager.IsValid()
        ? World.StrateManager->GetNumStrates()
        : 0;
    if (!bPerturbedRebuild || NumPerturbedStrates != NumPoolDefinitions)
    {
        AddError(TEXT("Changing the early connection count prevented the known layout from ")
                 TEXT("rebuilding; the passage-independence check could not run."));
        EarlyDefinition->PassageConfig.Connections = OriginalConnections;
        return false;
    }
    const FLayoutSnapshot Perturbed = CaptureSnapshot(*World.StrateManager.Get());
    EarlyDefinition->PassageConfig.Connections = OriginalConnections;

    FString LayoutMismatch;
    if (!SameLayout(Snapshots[0].Layout, Perturbed.Layout, LayoutMismatch))
    {
        AddError(FString::Printf(
            TEXT("Changing only the early connection count changed the strate layout: %s"),
            *LayoutMismatch));
    }

    for (int32 BoundaryIndex = 1; BoundaryIndex < Snapshots[0].Layout.Num() - 1; ++BoundaryIndex)
    {
        TArray<const FVoxelPassage*> ReferenceBoundary;
        TArray<const FVoxelPassage*> PerturbedBoundary;
        for (const FVoxelPassage& Passage : Snapshots[0].Passages)
        {
            if (Passage.UpperStrateIndex == BoundaryIndex)
            {
                ReferenceBoundary.Add(&Passage);
            }
        }
        for (const FVoxelPassage& Passage : Perturbed.Passages)
        {
            if (Passage.UpperStrateIndex == BoundaryIndex)
            {
                PerturbedBoundary.Add(&Passage);
            }
        }

        ++NumPassagesInspected;
        if (ReferenceBoundary.Num() != PerturbedBoundary.Num())
        {
            AddError(FString::Printf(
                TEXT("Boundary %d passage count changed after an earlier connection count ")
                TEXT("was perturbed (%d vs %d)."),
                BoundaryIndex, ReferenceBoundary.Num(), PerturbedBoundary.Num()));
            continue;
        }

        for (int32 PassageIndex = 0; PassageIndex < ReferenceBoundary.Num(); ++PassageIndex)
        {
            FString Mismatch;
            if (!SamePassage(*ReferenceBoundary[PassageIndex], *PerturbedBoundary[PassageIndex],
                             Mismatch, PassageIndex))
            {
                AddError(FString::Printf(
                    TEXT("Passage at boundary %d changed when an earlier connection count was ")
                    TEXT("perturbed: %s"),
                    BoundaryIndex, *Mismatch));
            }
        }
    }

    if (NumSlotsInspected == 0 || NumPassagesInspected == 0)
    {
        AddError(FString::Printf(
            TEXT("The test inspected no comparable data (slots=%d, passages=%d); this is a ")
            TEXT("test-harness failure, not a determinism pass."),
            NumSlotsInspected, NumPassagesInspected));
        return false;
    }

    AddInfo(FString::Printf(
        TEXT("Compared %d layout slots and %d passages across %d pool orders (seed=%d)."),
        NumSlotsInspected, NumPassagesInspected, Snapshots.Num(), LayoutSeed));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
