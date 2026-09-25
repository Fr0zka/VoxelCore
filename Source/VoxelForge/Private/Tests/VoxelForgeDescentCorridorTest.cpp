// Samples the authored A-to-B passage as a body-sized walking corridor in the final owner field.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Async/ParallelFor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#include "VoxelDiffLayer.h"
#include "VoxelDensityAblation.h"
#include "VoxelGenerator.h"
#include "VoxelCaveMorphology.h"
#include "VoxelPassageGeometry.h"
#include "VoxelSettings.h"
#include "VoxelStrateDefinition.h"
#include "VoxelStrateManager.h"
#include "VoxelTerrainOpDefinition.h"

namespace
{
    struct FOwnerPassageWorld
    {
        TStrongObjectPtr<UVoxelSettings> Settings;
        TStrongObjectPtr<UVoxelStrateManager> Manager;
        TStrongObjectPtr<UVoxelDiffLayer> DiffLayer;
        TStrongObjectPtr<UVoxelGenerator> Generator;
        TArray<TStrongObjectPtr<UVoxelStrateDefinition>> Definitions;

        bool Build(int32 Seed, FString& OutError)
        {
            UVoxelSettings* AuthoredSettings = LoadObject<UVoxelSettings>(
                nullptr, TEXT("/Game/VoxelForge/DA_Settings.DA_Settings"));
            if (AuthoredSettings == nullptr)
            {
                OutError = TEXT("Could not load owner asset /Game/VoxelForge/DA_Settings.DA_Settings");
                return false;
            }

            Settings = TStrongObjectPtr<UVoxelSettings>(DuplicateObject<UVoxelSettings>(
                AuthoredSettings, GetTransientPackage()));
            if (!Settings.IsValid())
            {
                OutError = TEXT("Could not duplicate owner settings into the transient package");
                return false;
            }

            Settings->Seed = Seed;
            Settings->Season.Reset();
            Settings->StratePool.Reset();
            Settings->FixedStrates.Reset();
            for (int32 Index = 0; Index < 4; ++Index)
            {
                const FString AssetPath = FString::Printf(
                    TEXT("/Game/VoxelForge/DA_Strate%d.DA_Strate%d"), Index + 1, Index + 1);
                UVoxelStrateDefinition* Definition = LoadObject<UVoxelStrateDefinition>(
                    nullptr, *AssetPath);
                if (Definition == nullptr)
                {
                    OutError = FString::Printf(TEXT("Could not load owner asset %s"), *AssetPath);
                    return false;
                }
                Definitions.Add(TStrongObjectPtr<UVoxelStrateDefinition>(Definition));
                Settings->FixedStrates.Add(Index, TSoftObjectPtr<UVoxelStrateDefinition>(Definition));
            }
            Settings->TotalStrates = Definitions.Num();

            Manager = TStrongObjectPtr<UVoxelStrateManager>(
                NewObject<UVoxelStrateManager>(GetTransientPackage()));
            if (!Manager.IsValid() || !Manager->Initialize(Settings.Get(), Seed))
            {
                OutError = FString::Printf(TEXT("Owner strate manager initialization failed for seed %d"), Seed);
                return false;
            }

            DiffLayer = TStrongObjectPtr<UVoxelDiffLayer>(
                NewObject<UVoxelDiffLayer>(GetTransientPackage()));
            Generator = TStrongObjectPtr<UVoxelGenerator>(
                NewObject<UVoxelGenerator>(GetTransientPackage()));
            if (!DiffLayer.IsValid() || !Generator.IsValid())
            {
                OutError = TEXT("Could not create the owner field generator or diff layer");
                return false;
            }
            Generator->InitializeSettings(Settings.Get());
            Generator->SetStrateManager(Manager.Get());
            Generator->SetDiffLayer(DiffLayer.Get());
            return true;
        }
    };

    struct FWalkCorridorSamples
    {
        TArray<FVector> Positions;
        TArray<float> Densities;
        TArray<int32> SampleStations;
        TArray<int32> SampleLaterals;
        TArray<int32> SampleHeights;
        int32 SolidSamples = 0;
        int32 BlockingSolidSamples = 0;
        int32 SolidStations = 0;
        int32 BlockingSolidStations = 0;
        int32 FloorStations = 0;
        int32 MissingFloorStations = 0;
        int32 MaxMissingFloorRunStations = 0;
        FVector FirstMissingFloorPosition = FVector::ZeroVector;
        float FirstMissingFloorExpectedZ = 0.0f;
        int32 FirstMissingFloorStation = INDEX_NONE;
        int32 WidthFloorGaps = 0;
        int32 StepViolations = 0;
        int32 FloorProfileViolations = 0;
        float MaxLateralFloorDeltaVoxels = 0.0f;
        float MaxFloorProfileDeviationVoxels = 0.0f;
        TArray<int32> SolidSamplesByHeight;
        float MaxStepVoxels = 0.0f;
        FVector FirstStepStart = FVector::ZeroVector;
        FVector FirstStepEnd = FVector::ZeroVector;
        float FirstStepExpectedStartZ = 0.0f;
        float FirstStepExpectedEndZ = 0.0f;
        FVector FirstSolidPosition = FVector::ZeroVector;
        float FirstSolidDensity = 0.0f;
        float FirstSolidMeasuredFloorZ = 0.0f;
        float FirstSolidExpectedFloorZ = 0.0f;
        int32 FirstSolidStation = INDEX_NONE;
        int32 FirstSolidLateralIndex = INDEX_NONE;
        int32 FirstSolidHeightIndex = INDEX_NONE;
        FVector FirstTallSolidPosition = FVector::ZeroVector;
        float FirstTallSolidDensity = 0.0f;
        int32 FirstTallSolidStation = INDEX_NONE;
        int32 FirstTallSolidLateralIndex = INDEX_NONE;
        int32 FirstTallSolidHeightIndex = INDEX_NONE;
        FVector FirstBlockingSolidPosition = FVector::ZeroVector;
        FVector FirstBlockingFeetPosition = FVector::ZeroVector;
        float FirstBlockingSolidDensity = 0.0f;
        int32 FirstBlockingSolidStation = INDEX_NONE;
        int32 FirstBlockingSolidLateralIndex = INDEX_NONE;
        int32 FirstBlockingSolidHeightIndex = INDEX_NONE;
        uint32 PositionHash = 0;
        uint32 DensityHash = 0;
    };

    bool BuildWalkCorridorSamples(
        const FOwnerPassageWorld& World,
        int32 Seed,
        FWalkCorridorSamples& OutSamples,
        FString& OutError)
    {
        const TArray<FStrateSlot>& Layout = World.Manager->GetLayout();
        const FVoxelPassage* Passage = nullptr;
        for (const FVoxelPassage& Candidate : World.Manager->GetPassages())
        {
            if (Candidate.bWalkableTunnelContract
                && Candidate.UpperStrateIndex == 0
                && Candidate.LowerStrateIndex == 1
                && Candidate.ControlPoints.Num() >= 2
                && Candidate.ControlRadii.Num() == Candidate.ControlPoints.Num()
                && Candidate.NativeFloorProfileZ.Num() == Candidate.ControlPoints.Num())
            {
                Passage = &Candidate;
                break;
            }
        }
        if (Passage == nullptr || !Layout.IsValidIndex(0) || !Layout.IsValidIndex(1))
        {
            OutError = FString::Printf(TEXT("No usable owner A-to-B walkable passage for seed %d"), Seed);
            return false;
        }

        // The construction-time floor profile locates the expected route. The actual support
        // height is traced from final density below, so a walkable floor bump is not misreported
        // as a pillar intersecting a body held at an obsolete profile height.
        TArray<FVector> FloorRoute;
        FloorRoute.Reserve(Passage->ControlPoints.Num() + 4);
        FloorRoute.Add(FVector(
            Passage->UpperLanding.StandingPoint.X,
            Passage->UpperLanding.StandingPoint.Y,
            Passage->UpperLanding.FloorZ));
        FloorRoute.Add(FVector(
            Passage->UpperLanding.DoorPoint.X,
            Passage->UpperLanding.DoorPoint.Y,
            Passage->UpperLanding.FloorZ));
        for (int32 Index = 0; Index < Passage->ControlPoints.Num(); ++Index)
        {
            const FVector& Control = Passage->ControlPoints[Index];
            FloorRoute.Add(FVector(
                Control.X, Control.Y, Passage->NativeFloorProfileZ[Index]));
        }
        FloorRoute.Add(FVector(
            Passage->LowerLanding.DoorPoint.X,
            Passage->LowerLanding.DoorPoint.Y,
            Passage->LowerLanding.FloorZ));
        FloorRoute.Add(FVector(
            Passage->LowerLanding.StandingPoint.X,
            Passage->LowerLanding.StandingPoint.Y,
            Passage->LowerLanding.FloorZ));

        constexpr float MaxStationSpacing = 0.5f;
        constexpr float PlayerWidthVoxels =
            VoxelPassageGeometry::WalkCorridorPlayerWidthVoxels;
        constexpr float PlayerHalfWidthVoxels = PlayerWidthVoxels * 0.5f;
        constexpr float PlayerHeightVoxels =
            VoxelPassageGeometry::WalkCorridorPlayerHeightVoxels;
        constexpr int32 NumLateralSamples = 5;
        constexpr int32 CenterLateralIndex = NumLateralSamples / 2;
        constexpr float LateralSampleSpacing =
            PlayerWidthVoxels / static_cast<float>(NumLateralSamples - 1);
        constexpr int32 NumVerticalSamples = 14;
        constexpr int32 FloorProbeIntervals = 128;
        constexpr float FloorProbeHalfRange = 32.0f;
        constexpr float FloorProbeStep =
            (2.0f * FloorProbeHalfRange) / static_cast<float>(FloorProbeIntervals);
        constexpr float FirstBodySampleZ = 0.25f;
        constexpr float BodySampleSpacing = 0.5f;
        TArray<FVector> FeetStations;
        TArray<FVector> LateralAxes;
        TArray<float> ExpectedFloorZ;
        for (int32 SegmentIndex = 0; SegmentIndex + 1 < FloorRoute.Num(); ++SegmentIndex)
        {
            const FVector Start = FloorRoute[SegmentIndex];
            const FVector End = FloorRoute[SegmentIndex + 1];
            const FVector Delta = End - Start;
            const float SegmentLength = Delta.Size();
            if (!(SegmentLength > KINDA_SMALL_NUMBER))
            {
                continue;
            }
            FVector Lateral(-Delta.Y, Delta.X, 0.0f);
            if (!Lateral.Normalize())
            {
                // A vertical segment has no horizontal travel axis. Keep a fixed perpendicular
                // plane there; the adjacent non-vertical segment samples the turn itself.
                Lateral = FVector(1.0f, 0.0f, 0.0f);
            }
            const int32 NumIntervals = FMath::Max(
                1, FMath::CeilToInt(SegmentLength / MaxStationSpacing));
            for (int32 Station = 0; Station <= NumIntervals; ++Station)
            {
                if (SegmentIndex > 0 && Station == 0)
                {
                    continue;
                }
                const float Alpha = static_cast<float>(Station)
                    / static_cast<float>(NumIntervals);
                const FVector Feet = FMath::Lerp(Start, End, Alpha);
                FeetStations.Add(Feet);
                LateralAxes.Add(Lateral);
                ExpectedFloorZ.Add(Feet.Z);
            }
        }

        if (FeetStations.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Owner A-to-B corridor was empty for seed %d"), Seed);
            return false;
        }

        TArray<FVector> FloorProbePositions;
        FloorProbePositions.Reserve(
            FeetStations.Num() * NumLateralSamples * (FloorProbeIntervals + 1));
        for (int32 StationIndex = 0; StationIndex < FeetStations.Num(); ++StationIndex)
        {
            for (int32 LateralIndex = 0; LateralIndex < NumLateralSamples; ++LateralIndex)
            {
                const float Side = -PlayerHalfWidthVoxels
                    + static_cast<float>(LateralIndex) * LateralSampleSpacing;
                const FVector LateralPosition = FeetStations[StationIndex]
                    + LateralAxes[StationIndex] * Side;
                for (int32 ProbeIndex = 0; ProbeIndex <= FloorProbeIntervals; ++ProbeIndex)
                {
                    const float Offset = -FloorProbeHalfRange
                        + static_cast<float>(ProbeIndex) * FloorProbeStep;
                    FloorProbePositions.Add(
                        LateralPosition + FVector(0.0f, 0.0f, Offset));
                }
            }
        }
        TArray<float> FloorProbeDensities;
        FloorProbeDensities.SetNumUninitialized(FloorProbePositions.Num());
        ParallelFor(FloorProbePositions.Num(), [&](int32 ProbeIndex)
        {
            const FVector& Position = FloorProbePositions[ProbeIndex];
            FloorProbeDensities[ProbeIndex] = World.Generator->GetDensityAt(
                Position.X, Position.Y, Position.Z);
        });

        TArray<float> MeasuredFloorZ;
        MeasuredFloorZ.SetNumUninitialized(FeetStations.Num() * NumLateralSamples);
        TArray<uint8> bHasMeasuredFloor;
        bHasMeasuredFloor.Init(0, FeetStations.Num() * NumLateralSamples);
        OutSamples.FloorStations = 0;
        OutSamples.MissingFloorStations = 0;
        OutSamples.WidthFloorGaps = 0;
        OutSamples.FloorProfileViolations = 0;
        OutSamples.MaxLateralFloorDeltaVoxels = 0.0f;
        OutSamples.MaxFloorProfileDeviationVoxels = 0.0f;
        OutSamples.StepViolations = 0;
        OutSamples.MaxMissingFloorRunStations = 0;
        OutSamples.MaxStepVoxels = 0.0f;
        OutSamples.FirstStepStart = FVector::ZeroVector;
        OutSamples.FirstStepEnd = FVector::ZeroVector;
        OutSamples.FirstStepExpectedStartZ = 0.0f;
        OutSamples.FirstStepExpectedEndZ = 0.0f;
        int32 CurrentMissingFloorRunStations = 0;
        for (int32 StationIndex = 0; StationIndex < FeetStations.Num(); ++StationIndex)
        {
            for (int32 LateralIndex = 0; LateralIndex < NumLateralSamples; ++LateralIndex)
            {
                const int32 FloorIndex = StationIndex * NumLateralSamples + LateralIndex;
                const int32 ProbeBase = FloorIndex * (FloorProbeIntervals + 1);
                float BestDistance = FLT_MAX;
                float BestFloor = ExpectedFloorZ[StationIndex];
                for (int32 ProbeIndex = 0; ProbeIndex < FloorProbeIntervals; ++ProbeIndex)
                {
                    const float BelowDensity = FloorProbeDensities[ProbeBase + ProbeIndex];
                    const float AboveDensity = FloorProbeDensities[ProbeBase + ProbeIndex + 1];
                    if (!FMath::IsFinite(BelowDensity) || !FMath::IsFinite(AboveDensity))
                    {
                        OutError = FString::Printf(
                            TEXT("Non-finite floor probe density at seed %d station %d lateral %d"),
                            Seed, StationIndex, LateralIndex);
                        return false;
                    }
                    if (!(BelowDensity < 0.0f) || !(AboveDensity >= 0.0f))
                    {
                        continue;
                    }
                    const float Denominator = AboveDensity - BelowDensity;
                    const float Alpha = Denominator > 0.0f
                        ? FMath::Clamp(-BelowDensity / Denominator, 0.0f, 1.0f)
                        : 0.0f;
                    const float CrossingZ = FeetStations[StationIndex].Z
                        - FloorProbeHalfRange
                        + (static_cast<float>(ProbeIndex) + Alpha) * FloorProbeStep;
                    const float Distance = FMath::Abs(
                        CrossingZ - ExpectedFloorZ[StationIndex]);
                    if (Distance < BestDistance)
                    {
                        BestDistance = Distance;
                        BestFloor = CrossingZ;
                    }
                }

                MeasuredFloorZ[FloorIndex] = BestFloor;
                if (BestDistance != FLT_MAX)
                {
                    bHasMeasuredFloor[FloorIndex] = 1;
                    if (LateralIndex == CenterLateralIndex)
                    {
                        ++OutSamples.FloorStations;
                        const float ProfileDeviation = FMath::Abs(
                            BestFloor - ExpectedFloorZ[StationIndex]);
                        OutSamples.MaxFloorProfileDeviationVoxels = FMath::Max(
                            OutSamples.MaxFloorProfileDeviationVoxels, ProfileDeviation);
                        if (ProfileDeviation > 1.8f + KINDA_SMALL_NUMBER)
                        {
                            ++OutSamples.FloorProfileViolations;
                        }
                    }
                }
                else if (LateralIndex == CenterLateralIndex)
                {
                    ++OutSamples.MissingFloorStations;
                    if (OutSamples.FirstMissingFloorStation == INDEX_NONE)
                    {
                        OutSamples.FirstMissingFloorStation = StationIndex;
                        OutSamples.FirstMissingFloorPosition = FeetStations[StationIndex];
                        OutSamples.FirstMissingFloorExpectedZ = ExpectedFloorZ[StationIndex];
                    }
                    ++CurrentMissingFloorRunStations;
                    OutSamples.MaxMissingFloorRunStations = FMath::Max(
                        OutSamples.MaxMissingFloorRunStations,
                        CurrentMissingFloorRunStations);
                }
                else
                {
                    ++OutSamples.WidthFloorGaps;
                }
            }

            const int32 CenterFloorIndex =
                StationIndex * NumLateralSamples + CenterLateralIndex;
            if (bHasMeasuredFloor[CenterFloorIndex])
            {
                CurrentMissingFloorRunStations = 0;
            }
            for (int32 LateralIndex : { 0, NumLateralSamples - 1 })
            {
                const int32 SideFloorIndex = StationIndex * NumLateralSamples + LateralIndex;
                if (bHasMeasuredFloor[CenterFloorIndex]
                    && bHasMeasuredFloor[SideFloorIndex])
                {
                    OutSamples.MaxLateralFloorDeltaVoxels = FMath::Max(
                        OutSamples.MaxLateralFloorDeltaVoxels,
                        FMath::Abs(
                            MeasuredFloorZ[SideFloorIndex]
                            - MeasuredFloorZ[CenterFloorIndex]));
                }
            }

            if (StationIndex > 0
                && bHasMeasuredFloor[CenterFloorIndex]
                && bHasMeasuredFloor[
                    (StationIndex - 1) * NumLateralSamples + CenterLateralIndex])
            {
                const int32 PreviousCenterFloorIndex =
                    (StationIndex - 1) * NumLateralSamples + CenterLateralIndex;
                const float Step = FMath::Abs(
                    MeasuredFloorZ[CenterFloorIndex]
                    - MeasuredFloorZ[PreviousCenterFloorIndex]);
                OutSamples.MaxStepVoxels = FMath::Max(OutSamples.MaxStepVoxels, Step);
                if (Step > 1.8f + KINDA_SMALL_NUMBER)
                {
                    if (OutSamples.StepViolations == 0)
                    {
                        OutSamples.FirstStepStart = FVector(
                            FeetStations[StationIndex - 1].X,
                            FeetStations[StationIndex - 1].Y,
                            MeasuredFloorZ[PreviousCenterFloorIndex]);
                        OutSamples.FirstStepEnd = FVector(
                            FeetStations[StationIndex].X,
                            FeetStations[StationIndex].Y,
                            MeasuredFloorZ[CenterFloorIndex]);
                        OutSamples.FirstStepExpectedStartZ =
                            ExpectedFloorZ[StationIndex - 1];
                        OutSamples.FirstStepExpectedEndZ =
                            ExpectedFloorZ[StationIndex];
                    }
                    ++OutSamples.StepViolations;
                }
            }
        }

        if (Seed == 0 && OutSamples.FirstMissingFloorStation != INDEX_NONE)
        {
            constexpr float GapProfileMinOffset = -360.0f;
            constexpr float GapProfileMaxOffset = 8.0f;
            constexpr float GapProfileStep = 0.5f;
            constexpr int32 GapProfileSamples = 737;
            TArray<float> GapProfileDensities;
            GapProfileDensities.SetNumUninitialized(GapProfileSamples);
            const FVector& GapPosition = OutSamples.FirstMissingFloorPosition;
            const float GapExpectedZ = OutSamples.FirstMissingFloorExpectedZ;
            ParallelFor(GapProfileSamples, [&](int32 ProfileIndex)
            {
                const float Offset = GapProfileMinOffset
                    + static_cast<float>(ProfileIndex) * GapProfileStep;
                GapProfileDensities[ProfileIndex] = World.Generator->GetDensityAt(
                    GapPosition.X, GapPosition.Y, GapExpectedZ + Offset);
            });
            TArray<float> GapFloorCrossingOffsets;
            for (int32 ProfileIndex = 0;
                 ProfileIndex + 1 < GapProfileDensities.Num();
                 ++ProfileIndex)
            {
                const float BelowDensity = GapProfileDensities[ProfileIndex];
                const float AboveDensity = GapProfileDensities[ProfileIndex + 1];
                if (BelowDensity < 0.0f && AboveDensity >= 0.0f)
                {
                    const float Alpha = -BelowDensity / (AboveDensity - BelowDensity);
                    GapFloorCrossingOffsets.Add(
                        GapProfileMinOffset
                        + (static_cast<float>(ProfileIndex) + Alpha) * GapProfileStep);
                }
            }
            const float DensityAtMinus34093 = World.Generator->GetDensityAt(
                GapPosition.X, GapPosition.Y, GapExpectedZ - 340.93f);
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeDescentGapProfile] seed=0 station=%d position=(%.3f,%.3f,%.3f) "
                     "expected_z=%.3f density_at_minus340_93=%.6f floor_crossings_from_expected=[%s]"),
                OutSamples.FirstMissingFloorStation,
                GapPosition.X, GapPosition.Y, GapExpectedZ,
                GapExpectedZ,
                DensityAtMinus34093,
                *FString::JoinBy(GapFloorCrossingOffsets, TEXT(","),
                    [](float Offset) { return FString::Printf(TEXT("%.3f"), Offset); }));
        }

        OutSamples.Positions.Reset(FeetStations.Num() * NumLateralSamples * NumVerticalSamples);
        OutSamples.SampleStations.Reset(
            FeetStations.Num() * NumLateralSamples * NumVerticalSamples);
        OutSamples.SampleLaterals.Reset(
            FeetStations.Num() * NumLateralSamples * NumVerticalSamples);
        OutSamples.SampleHeights.Reset(
            FeetStations.Num() * NumLateralSamples * NumVerticalSamples);
        for (int32 StationIndex = 0; StationIndex < FeetStations.Num(); ++StationIndex)
        {
            const FVector& Lateral = LateralAxes[StationIndex];
            for (int32 LateralIndex = 0; LateralIndex < NumLateralSamples; ++LateralIndex)
            {
                FVector Feet = FeetStations[StationIndex];
                // Anchor the upright player-sized stencil to the measured centerline floor. A
                // nearby raised edge can be stepped over up to the contract's 1.8-voxel limit.
                const int32 CenterFloorIndex =
                    StationIndex * NumLateralSamples + CenterLateralIndex;
                Feet.Z = bHasMeasuredFloor[CenterFloorIndex]
                    ? MeasuredFloorZ[CenterFloorIndex]
                    : ExpectedFloorZ[StationIndex];
                for (int32 VerticalIndex = 0; VerticalIndex < NumVerticalSamples; ++VerticalIndex)
                {
                    const float Height = FirstBodySampleZ
                        + static_cast<float>(VerticalIndex) * BodySampleSpacing;
                    const float DistanceFromCapCenter = Height < PlayerHalfWidthVoxels
                        ? PlayerHalfWidthVoxels - Height
                        : (Height > PlayerHeightVoxels
                            - PlayerHalfWidthVoxels
                            ? Height - (PlayerHeightVoxels
                                - PlayerHalfWidthVoxels)
                            : 0.0f);
                    const float SliceRadius = FMath::Sqrt(FMath::Max(
                        FMath::Square(PlayerHalfWidthVoxels)
                            - FMath::Square(DistanceFromCapCenter),
                        0.0f));
                    const float BodySide = SliceRadius
                        * (static_cast<float>(LateralIndex) / 2.0f - 1.0f);
                    OutSamples.Positions.Add(
                        Feet + Lateral * BodySide + FVector(0.0f, 0.0f, Height));
                    OutSamples.SampleStations.Add(StationIndex);
                    OutSamples.SampleLaterals.Add(LateralIndex);
                    OutSamples.SampleHeights.Add(VerticalIndex);
                }
            }
        }

        OutSamples.Densities.SetNumUninitialized(OutSamples.Positions.Num());
        ParallelFor(OutSamples.Positions.Num(), [&](int32 SampleIndex)
        {
            const FVector& Position = OutSamples.Positions[SampleIndex];
            OutSamples.Densities[SampleIndex] = World.Generator->GetDensityAt(
                Position.X, Position.Y, Position.Z);
        });

        OutSamples.SolidSamples = 0;
        OutSamples.BlockingSolidSamples = 0;
        OutSamples.SolidStations = 0;
        OutSamples.BlockingSolidStations = 0;
        OutSamples.SolidSamplesByHeight.Init(0, NumVerticalSamples);
        OutSamples.FirstSolidPosition = FVector::ZeroVector;
        OutSamples.FirstSolidDensity = 0.0f;
        OutSamples.FirstSolidStation = INDEX_NONE;
        OutSamples.FirstSolidLateralIndex = INDEX_NONE;
        OutSamples.FirstSolidHeightIndex = INDEX_NONE;
        OutSamples.FirstTallSolidPosition = FVector::ZeroVector;
        OutSamples.FirstTallSolidDensity = 0.0f;
        OutSamples.FirstTallSolidStation = INDEX_NONE;
        OutSamples.FirstTallSolidLateralIndex = INDEX_NONE;
        OutSamples.FirstTallSolidHeightIndex = INDEX_NONE;
        OutSamples.FirstBlockingSolidPosition = FVector::ZeroVector;
        OutSamples.FirstBlockingSolidDensity = 0.0f;
        OutSamples.FirstBlockingSolidStation = INDEX_NONE;
        OutSamples.FirstBlockingSolidLateralIndex = INDEX_NONE;
        OutSamples.FirstBlockingSolidHeightIndex = INDEX_NONE;
        TSet<int32> SolidStationIds;
        TSet<int32> BlockingSolidStationIds;
        for (int32 SampleIndex = 0; SampleIndex < OutSamples.Densities.Num(); ++SampleIndex)
        {
            const float Density = OutSamples.Densities[SampleIndex];
            if (FMath::IsFinite(Density) && Density < 0.0f)
            {
                if (OutSamples.SolidSamples == 0)
                {
                    OutSamples.FirstSolidPosition = OutSamples.Positions[SampleIndex];
                    OutSamples.FirstSolidDensity = Density;
                    OutSamples.FirstSolidStation = OutSamples.SampleStations[SampleIndex];
                    OutSamples.FirstSolidLateralIndex = OutSamples.SampleLaterals[SampleIndex];
                    OutSamples.FirstSolidHeightIndex = OutSamples.SampleHeights[SampleIndex];
                    OutSamples.FirstSolidMeasuredFloorZ =
                        MeasuredFloorZ[
                            OutSamples.FirstSolidStation * NumLateralSamples
                            + OutSamples.FirstSolidLateralIndex];
                    OutSamples.FirstSolidExpectedFloorZ =
                        ExpectedFloorZ[OutSamples.FirstSolidStation];
                }
                if (OutSamples.FirstTallSolidStation == INDEX_NONE
                    && OutSamples.SampleHeights[SampleIndex] > 0)
                {
                    OutSamples.FirstTallSolidPosition = OutSamples.Positions[SampleIndex];
                    OutSamples.FirstTallSolidDensity = Density;
                    OutSamples.FirstTallSolidStation = OutSamples.SampleStations[SampleIndex];
                    OutSamples.FirstTallSolidLateralIndex = OutSamples.SampleLaterals[SampleIndex];
                    OutSamples.FirstTallSolidHeightIndex = OutSamples.SampleHeights[SampleIndex];
                }
                ++OutSamples.SolidSamples;
                ++OutSamples.SolidSamplesByHeight[SampleIndex % NumVerticalSamples];
                // Each station owns a fixed block of samples. The compact index is enough for an
                // exact count of blocked positions without retaining another per-sample array.
                SolidStationIds.Add(OutSamples.SampleStations[SampleIndex]);
                const int32 StationIndex = OutSamples.SampleStations[SampleIndex];
                const int32 LateralIndex = OutSamples.SampleLaterals[SampleIndex];
                const int32 CenterFloorIndex =
                    StationIndex * NumLateralSamples + CenterLateralIndex;
                const float SupportFloorZ = bHasMeasuredFloor[CenterFloorIndex]
                    ? MeasuredFloorZ[CenterFloorIndex]
                    : ExpectedFloorZ[StationIndex];
                if (OutSamples.Positions[SampleIndex].Z - SupportFloorZ
                    > 1.8f + KINDA_SMALL_NUMBER)
                {
                    if (OutSamples.FirstBlockingSolidStation == INDEX_NONE)
                    {
                        OutSamples.FirstBlockingSolidPosition = OutSamples.Positions[SampleIndex];
                        OutSamples.FirstBlockingFeetPosition = FeetStations[StationIndex];
                        OutSamples.FirstBlockingFeetPosition.Z = SupportFloorZ;
                        OutSamples.FirstBlockingSolidDensity = Density;
                        OutSamples.FirstBlockingSolidStation = StationIndex;
                        OutSamples.FirstBlockingSolidLateralIndex = LateralIndex;
                        OutSamples.FirstBlockingSolidHeightIndex =
                            OutSamples.SampleHeights[SampleIndex];
                    }
                    ++OutSamples.BlockingSolidSamples;
                    BlockingSolidStationIds.Add(StationIndex);
                }
            }
            else if (!FMath::IsFinite(Density))
            {
                OutError = FString::Printf(
                    TEXT("Non-finite owner density at seed %d sample %d"), Seed, SampleIndex);
                return false;
            }
        }
        OutSamples.SolidStations = SolidStationIds.Num();
        OutSamples.BlockingSolidStations = BlockingSolidStationIds.Num();
        OutSamples.PositionHash = FCrc::MemCrc32(
            OutSamples.Positions.GetData(), OutSamples.Positions.Num() * sizeof(FVector));
        OutSamples.DensityHash = FCrc::MemCrc32(
            OutSamples.Densities.GetData(), OutSamples.Densities.Num() * sizeof(float));
        return true;
    }

    bool SampleOwnerSeed(
        int32 Seed,
        FWalkCorridorSamples& OutSamples,
        FString& OutError)
    {
        FOwnerPassageWorld World;
        if (!World.Build(Seed, OutError))
        {
            return false;
        }
        if (!BuildWalkCorridorSamples(World, Seed, OutSamples, OutError))
        {
            return false;
        }
        if (Seed == 0 && OutSamples.SolidSamples > 0)
        {
            const FIntVector ChunkCoord(
                FMath::FloorToInt(OutSamples.FirstSolidPosition.X / static_cast<float>(CHUNK_SIZE)),
                FMath::FloorToInt(OutSamples.FirstSolidPosition.Y / static_cast<float>(CHUNK_SIZE)),
                FMath::FloorToInt(OutSamples.FirstSolidPosition.Z / static_cast<float>(CHUNK_SIZE)));
            const UVoxelStrateDefinition* Definition = World.Manager->GetStrateForChunk(ChunkCoord);
            const FStrateGenerationParams Params = World.Manager->GetGenerationParams(ChunkCoord);
            FString TerrainOps;
            FString TerrainOpPaths;
            if (Definition != nullptr)
            {
                for (const FStrateTerrainOpEntry& Entry : Definition->TerrainOperations)
                {
                    TerrainOpPaths += FString::Printf(TEXT("%s "), *Entry.Operation.ToSoftObjectPath().ToString());
                    const UVoxelTerrainOpDefinition* Operation = Entry.Operation.LoadSynchronous();
                    if (Operation == nullptr)
                    {
                        TerrainOps += TEXT("<unresolved> ");
                        continue;
                    }
                    const UEnum* OperationEnum = StaticEnum<EVoxelTerrainOpType>();
                    const FString TypeName = OperationEnum != nullptr
                        ? OperationEnum->GetNameStringByValue(static_cast<int64>(Operation->Type))
                        : FString::Printf(TEXT("Type_%d"), static_cast<int32>(Operation->Type));
                    TerrainOps += FString::Printf(
                        TEXT("%s:%s@%.3f "), *Operation->GetName(), *TypeName, Entry.Weight);
                }
            }
            const UEnum* GeneratorEnum = StaticEnum<ECaveGeneratorType>();
            const ECaveGeneratorType GeneratorType =
                World.Manager->GetGeneratorTypeForChunk(ChunkCoord);
            const FString GeneratorName = GeneratorEnum != nullptr
                ? GeneratorEnum->GetNameStringByValue(static_cast<int64>(GeneratorType))
                : FString::Printf(TEXT("Type_%d"), static_cast<int32>(GeneratorType));
            const bool bUsesOperatorStack = World.Manager->UsesOperatorStackForChunk(ChunkCoord);
            const float PassageSDF = World.Manager->EvaluateModifierSDF(
                OutSamples.FirstSolidPosition.X,
                OutSamples.FirstSolidPosition.Y,
                OutSamples.FirstSolidPosition.Z);
            float SimulatedCarvedInternalDensity = -OutSamples.FirstSolidDensity;
            World.Manager->ApplyPassageCarvingOnly(
                SimulatedCarvedInternalDensity,
                OutSamples.FirstSolidPosition.X,
                OutSamples.FirstSolidPosition.Y,
                OutSamples.FirstSolidPosition.Z,
                Params.BaseDensity,
                Params.BoundarySealThickness);
            const float SimulatedCarvedFinalDensity = -SimulatedCarvedInternalDensity;
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeDescentWriterContext] seed=0 sample=(%.3f,%.3f,%.3f) "
                     "chunk=(%d,%d,%d) archetype=%s asset_stack_flag=%d effective_op_stack=%d "
                     "passage_sdf=%.6f simulated_passage_carve_density=%.6f "
                     "terrain_ops='%s' terrain_op_paths='%s' "
                     "base=%.3f tunnel_columns=(%.3f,%.3f,%.3f) slab_columns=(%.3f,%.3f,%.3f)"),
                OutSamples.FirstSolidPosition.X, OutSamples.FirstSolidPosition.Y,
                OutSamples.FirstSolidPosition.Z, ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z,
                *GeneratorName, Definition != nullptr && Definition->bUseOperatorStack ? 1 : 0,
                bUsesOperatorStack ? 1 : 0, PassageSDF, SimulatedCarvedFinalDensity,
                *TerrainOps, *TerrainOpPaths, Params.BaseDensity, Params.ColumnDensity,
                Params.ColumnMinRadius, Params.ColumnMaxRadius,
                Definition != nullptr ? Definition->SlabParams.ColumnDensity : -1.0f,
                Definition != nullptr ? Definition->SlabParams.ColumnMinRadius : -1.0f,
                Definition != nullptr ? Definition->SlabParams.ColumnMaxRadius : -1.0f);
        }

        if (Seed == 0)
        {
            // Keep the old owner-world obstruction as a fixed witness when launching a
            // process-level operator ablation. The ablation mask is intentionally latched once
            // per process, so diagnostic runs must set it on the command line before startup.
            const FVector BaselineWitness(2079.975f, -4457.087f, -1526.679f);
            const FIntVector WitnessChunk(
                FMath::FloorToInt(BaselineWitness.X / static_cast<float>(CHUNK_SIZE)),
                FMath::FloorToInt(BaselineWitness.Y / static_cast<float>(CHUNK_SIZE)),
                FMath::FloorToInt(BaselineWitness.Z / static_cast<float>(CHUNK_SIZE)));
            const FStrateGenerationParams WitnessParams =
                World.Manager->GetGenerationParams(WitnessChunk);
            const float WitnessDensity = World.Generator->GetDensityAt(
                BaselineWitness.X, BaselineWitness.Y, BaselineWitness.Z);
            const float WitnessPassageSDF = World.Manager->EvaluateModifierSDF(
                BaselineWitness.X, BaselineWitness.Y, BaselineWitness.Z);
            float WitnessCarvedInternalDensity = -WitnessDensity;
            World.Manager->ApplyPassageCarvingOnly(
                WitnessCarvedInternalDensity,
                BaselineWitness.X, BaselineWitness.Y, BaselineWitness.Z,
                WitnessParams.BaseDensity, WitnessParams.BoundarySealThickness);
            const float WitnessSimulatedCarveDensity = -WitnessCarvedInternalDensity;
            int32 WitnessPassageIndex = INDEX_NONE;
            bool bWitnessPassageNativeFloor = false;
            const TArray<FVoxelPassage>& Passages = World.Manager->GetPassages();
            for (int32 PassageIndex = 0; PassageIndex < Passages.Num(); ++PassageIndex)
            {
                const FVoxelPassage& Candidate = Passages[PassageIndex];
                if (Candidate.bWalkableTunnelContract
                    && Candidate.UpperStrateIndex == 0
                    && Candidate.LowerStrateIndex == 1
                    && Candidate.ControlPoints.Num() >= 2)
                {
                    WitnessPassageIndex = PassageIndex;
                    bWitnessPassageNativeFloor = Candidate.bNativeFloorEnabled;
                    break;
                }
            }
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeDescentFixedWitness] seed=0 position=(%.3f,%.3f,%.3f) "
                     "final_density=%.6f passage_sdf=%.6f simulated_carve_density=%.6f "
                     "a_to_b_passage_index=%d native_floor_enabled=%d"),
                BaselineWitness.X, BaselineWitness.Y, BaselineWitness.Z,
                WitnessDensity, WitnessPassageSDF, WitnessSimulatedCarveDensity,
                WitnessPassageIndex, bWitnessPassageNativeFloor ? 1 : 0);

            if (OutSamples.BlockingSolidSamples > 0)
            {
                const FVector Blocker = OutSamples.FirstBlockingSolidPosition;
                float NativeFloorFromAir = 64.0f;
                World.Manager->ApplyPassageNativeFloorMC(
                    NativeFloorFromAir, Blocker.X, Blocker.Y, Blocker.Z,
                    WitnessParams.BaseDensity);
                float MinRoomSDF = FLT_MAX;
                FString NearbySegments;
                FString ControlPoints;
                for (const FVoxelPassage& Candidate : Passages)
                {
                    if (Candidate.UpperStrateIndex != 0
                        || Candidate.LowerStrateIndex != 1
                        || Candidate.NativeFloorProfileZ.Num()
                            != Candidate.ControlPoints.Num())
                    {
                        continue;
                    }
                    MinRoomSDF = FMath::Min(MinRoomSDF,
                        FMath::Min(
                            VF_EvaluatePassageLandingSDF(Blocker, Candidate.UpperLanding),
                            VF_EvaluatePassageLandingSDF(Blocker, Candidate.LowerLanding)));
                    for (int32 ControlIndex = 0;
                         ControlIndex < Candidate.ControlPoints.Num();
                         ++ControlIndex)
                    {
                        const FVector& Control = Candidate.ControlPoints[ControlIndex];
                        ControlPoints += FString::Printf(
                            TEXT("cp%d=(%.1f,%.1f,%.1f;floor=%.1f,r=%.1f);"),
                            ControlIndex, Control.X, Control.Y, Control.Z,
                            Candidate.NativeFloorProfileZ[ControlIndex],
                            Candidate.ControlRadii.IsValidIndex(ControlIndex)
                                ? Candidate.ControlRadii[ControlIndex] : -1.0f);
                    }
                    for (int32 SegmentIndex = 0;
                         SegmentIndex + 1 < Candidate.ControlPoints.Num();
                         ++SegmentIndex)
                    {
                        const FVector& A = Candidate.ControlPoints[SegmentIndex];
                        const FVector& B = Candidate.ControlPoints[SegmentIndex + 1];
                        const float DX = B.X - A.X;
                        const float DY = B.Y - A.Y;
                        const float LengthSquared = DX * DX + DY * DY;
                        if (!(LengthSquared > KINDA_SMALL_NUMBER))
                        {
                            continue;
                        }
                        const float T = FMath::Clamp(
                            ((Blocker.X - A.X) * DX + (Blocker.Y - A.Y) * DY)
                                / LengthSquared,
                            0.0f, 1.0f);
                        const float ClosestX = A.X + DX * T;
                        const float ClosestY = A.Y + DY * T;
                        const float HorizontalDistance = FMath::Sqrt(
                            FMath::Square(Blocker.X - ClosestX)
                            + FMath::Square(Blocker.Y - ClosestY));
                        if (HorizontalDistance <= 2.0f)
                        {
                            const float FloorZ = FMath::Lerp(
                                Candidate.NativeFloorProfileZ[SegmentIndex],
                                Candidate.NativeFloorProfileZ[SegmentIndex + 1], T);
                            NearbySegments += FString::Printf(
                                TEXT("seg%d:xy=%.4f,floor=%.3f,offset=%.3f;"),
                                SegmentIndex, HorizontalDistance, FloorZ,
                                Blocker.Z - FloorZ);
                        }
                    }
                }
                UE_LOG(LogTemp, Display,
                    TEXT("[VoxelForgeDescentBlockerClearance] seed=0 blocker=(%.3f,%.3f,%.3f) "
                         "density=%.6f native_floor_from_air=%.3f min_room_sdf=%.3f segments='%s' controls='%s'"),
                    Blocker.X, Blocker.Y, Blocker.Z,
                    OutSamples.FirstBlockingSolidDensity, NativeFloorFromAir,
                    MinRoomSDF, *NearbySegments, *ControlPoints);
            }
        }
        else if ((Seed == 28 || Seed == 44 || Seed == 60
                || Seed == 61 || Seed == 63)
            && OutSamples.BlockingSolidSamples > 0)
        {
            const FVector Blocker = OutSamples.FirstBlockingSolidPosition;
            const FIntVector ChunkCoord(
                FMath::FloorToInt(Blocker.X / static_cast<float>(CHUNK_SIZE)),
                FMath::FloorToInt(Blocker.Y / static_cast<float>(CHUNK_SIZE)),
                FMath::FloorToInt(Blocker.Z / static_cast<float>(CHUNK_SIZE)));
            const FStrateGenerationParams Params =
                World.Manager->GetGenerationParams(ChunkCoord);
            const float FinalDensity = World.Generator->GetDensityAt(
                Blocker.X, Blocker.Y, Blocker.Z);
            const float PassageSDF = World.Manager->EvaluateModifierSDF(
                Blocker.X, Blocker.Y, Blocker.Z);
            float NativeFloorFromAir = 64.0f;
            World.Manager->ApplyPassageNativeFloorMC(
                NativeFloorFromAir, Blocker.X, Blocker.Y, Blocker.Z,
                Params.BaseDensity);
            float PassagePostsFromAir = 64.0f;
            World.Manager->ApplyPassageStructuralPostsMC(
                PassagePostsFromAir, Blocker.X, Blocker.Y, Blocker.Z,
                Params.BaseDensity, Params.BoundarySealThickness);
            float LandingFloorFromAir = 64.0f;
            World.Manager->ApplyPassageLandingFloorMC(
                LandingFloorFromAir, Blocker.X, Blocker.Y, Blocker.Z,
                Params.BaseDensity);
            float RoomFloorFromAir = 64.0f;
            World.Manager->ApplyPassageLandingRoomFloorMC(
                RoomFloorFromAir, Blocker.X, Blocker.Y, Blocker.Z,
                Params.BaseDensity);
            float MinRoomSDF = FLT_MAX;
            FString Segments;
            FString Controls;
            FString NearbyRoomSDFs;
            const TArray<FVoxelPassage>& AllPassages = World.Manager->GetPassages();
            for (int32 CandidateIndex = 0;
                 CandidateIndex < AllPassages.Num();
                 ++CandidateIndex)
            {
                const FVoxelPassage& Candidate = AllPassages[CandidateIndex];
                const float UpperRoomSDF = VF_EvaluatePassageLandingSDF(
                    Blocker, Candidate.UpperLanding);
                const float LowerRoomSDF = VF_EvaluatePassageLandingSDF(
                    Blocker, Candidate.LowerLanding);
                const float CandidateRoomSDF = FMath::Min(
                    UpperRoomSDF, LowerRoomSDF);
                if (CandidateRoomSDF < 16.0f)
                {
                    NearbyRoomSDFs += FString::Printf(
                        TEXT("p%d[%d>%d]=(u%.3f,l%.3f;uf%.2f,lf%.2f;"
                             "us%.1f,%.1f,ud%.1f,%.1f,ls%.1f,%.1f,ld%.1f,%.1f);"),
                        CandidateIndex, Candidate.UpperStrateIndex,
                        Candidate.LowerStrateIndex, UpperRoomSDF, LowerRoomSDF,
                        Candidate.UpperLanding.FloorZ, Candidate.LowerLanding.FloorZ,
                        Candidate.UpperLanding.StandingPoint.X,
                        Candidate.UpperLanding.StandingPoint.Y,
                        Candidate.UpperLanding.DoorPoint.X,
                        Candidate.UpperLanding.DoorPoint.Y,
                        Candidate.LowerLanding.StandingPoint.X,
                        Candidate.LowerLanding.StandingPoint.Y,
                        Candidate.LowerLanding.DoorPoint.X,
                        Candidate.LowerLanding.DoorPoint.Y);
                }
                if (Candidate.UpperStrateIndex != 0
                    || Candidate.LowerStrateIndex != 1
                    || Candidate.NativeFloorProfileZ.Num()
                        != Candidate.ControlPoints.Num())
                {
                    continue;
                }
                MinRoomSDF = FMath::Min(MinRoomSDF, CandidateRoomSDF);
                for (int32 ControlIndex = 0;
                     ControlIndex < Candidate.ControlPoints.Num();
                     ++ControlIndex)
                {
                    const FVector& Control = Candidate.ControlPoints[ControlIndex];
                    Controls += FString::Printf(
                        TEXT("cp%d=(%.1f,%.1f,%.1f;floor=%.1f,r=%.1f);"),
                        ControlIndex, Control.X, Control.Y, Control.Z,
                        Candidate.NativeFloorProfileZ[ControlIndex],
                        Candidate.ControlRadii.IsValidIndex(ControlIndex)
                            ? Candidate.ControlRadii[ControlIndex] : -1.0f);
                }
                for (int32 SegmentIndex = 0;
                     SegmentIndex + 1 < Candidate.ControlPoints.Num();
                     ++SegmentIndex)
                {
                    const FVector& A = Candidate.ControlPoints[SegmentIndex];
                    const FVector& B = Candidate.ControlPoints[SegmentIndex + 1];
                    const float DX = B.X - A.X;
                    const float DY = B.Y - A.Y;
                    const float LengthSquared = DX * DX + DY * DY;
                    if (!(LengthSquared > KINDA_SMALL_NUMBER))
                    {
                        continue;
                    }
                    const float T = FMath::Clamp(
                        ((Blocker.X - A.X) * DX + (Blocker.Y - A.Y) * DY)
                            / LengthSquared,
                        0.0f, 1.0f);
                    const float ClosestX = A.X + DX * T;
                    const float ClosestY = A.Y + DY * T;
                    const float HorizontalDistance = FMath::Sqrt(
                        FMath::Square(Blocker.X - ClosestX)
                        + FMath::Square(Blocker.Y - ClosestY));
                    if (HorizontalDistance <= 12.0f)
                    {
                        const float FloorZ = FMath::Lerp(
                            Candidate.NativeFloorProfileZ[SegmentIndex],
                            Candidate.NativeFloorProfileZ[SegmentIndex + 1], T);
                        Segments += FString::Printf(
                            TEXT("seg%d:xy=%.3f,floor=%.3f,offset=%.3f;"),
                            SegmentIndex, HorizontalDistance, FloorZ,
                            Blocker.Z - FloorZ);
                    }
                }
            }
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeDescentSeedBlocker] seed=%d point=(%.3f,%.3f,%.3f) "
                     "density=%.3f passage_sdf=%.3f native_floor_from_air=%.3f "
                     "passage_posts_from_air=%.3f landing_floor_from_air=%.3f "
                     "room_floor_from_air=%.3f room_sdf=%.3f rooms='%s' "
                     "segments='%s' controls='%s'"),
                Seed, Blocker.X, Blocker.Y, Blocker.Z,
                FinalDensity, PassageSDF, NativeFloorFromAir,
                PassagePostsFromAir, LandingFloorFromAir, RoomFloorFromAir,
                MinRoomSDF, *NearbyRoomSDFs, *Segments, *Controls);
        }
        return true;
    }

    void LogCorridorResult(
        FAutomationTestBase& Test,
        int32 Seed,
        const FWalkCorridorSamples& Samples)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeDescentCorridor] seed=%d samples=%d solid_samples=%d "
                 "blocked_samples=%d blocked_stations=%d floor_stations=%d missing_floor_stations=%d max_missing_run=%d "
                 "width_floor_gaps=%d max_lateral_floor_delta=%.3f "
                 "step_violations=%d max_step_vox=%.3f floor_profile_violations=%d max_floor_profile_deviation=%.3f "
                 "first_step=(%.3f,%.3f,%.3f)->(%.3f,%.3f,%.3f) "
                 "expected_step_floors=(%.3f->%.3f) "
                 "first_missing_floor=(station=%d,%.3f,%.3f,expected_z=%.3f) "
             "first_solid_vox=(%.3f,%.3f,%.3f) density=%.6f body_index=(station=%d,lateral=%d,height=%d) "
             "first_solid_floor=(measured=%.3f,expected=%.3f) "
             "first_tall_solid_vox=(%.3f,%.3f,%.3f) density=%.6f body_index=(station=%d,lateral=%d,height=%d) "
                         "first_blocking_solid_vox=(%.3f,%.3f,%.3f) density=%.6f feet=(%.3f,%.3f,%.3f) body_index=(station=%d,lateral=%d,height=%d) "
             "solid_by_height=[%s] "
                 "position_crc=%08X density_crc=%08X"),
            Seed, Samples.Densities.Num(), Samples.SolidSamples,
            Samples.BlockingSolidSamples, Samples.BlockingSolidStations,
            Samples.FloorStations, Samples.MissingFloorStations,
            Samples.MaxMissingFloorRunStations,
            Samples.WidthFloorGaps, Samples.MaxLateralFloorDeltaVoxels,
            Samples.StepViolations, Samples.MaxStepVoxels,
            Samples.FloorProfileViolations, Samples.MaxFloorProfileDeviationVoxels,
             Samples.FirstStepStart.X, Samples.FirstStepStart.Y, Samples.FirstStepStart.Z,
             Samples.FirstStepEnd.X, Samples.FirstStepEnd.Y, Samples.FirstStepEnd.Z,
             Samples.FirstStepExpectedStartZ, Samples.FirstStepExpectedEndZ,
            Samples.FirstMissingFloorStation,
            Samples.FirstMissingFloorPosition.X, Samples.FirstMissingFloorPosition.Y,
            Samples.FirstMissingFloorExpectedZ,
            Samples.FirstSolidPosition.X, Samples.FirstSolidPosition.Y,
            Samples.FirstSolidPosition.Z, Samples.FirstSolidDensity,
            Samples.FirstSolidStation, Samples.FirstSolidLateralIndex,
            Samples.FirstSolidHeightIndex,
            Samples.FirstSolidMeasuredFloorZ, Samples.FirstSolidExpectedFloorZ,
            Samples.FirstTallSolidPosition.X, Samples.FirstTallSolidPosition.Y,
            Samples.FirstTallSolidPosition.Z, Samples.FirstTallSolidDensity,
            Samples.FirstTallSolidStation, Samples.FirstTallSolidLateralIndex,
            Samples.FirstTallSolidHeightIndex,
            Samples.FirstBlockingSolidPosition.X, Samples.FirstBlockingSolidPosition.Y,
            Samples.FirstBlockingSolidPosition.Z, Samples.FirstBlockingSolidDensity,
            Samples.FirstBlockingFeetPosition.X, Samples.FirstBlockingFeetPosition.Y,
            Samples.FirstBlockingFeetPosition.Z,
            Samples.FirstBlockingSolidStation, Samples.FirstBlockingSolidLateralIndex,
            Samples.FirstBlockingSolidHeightIndex,
            *FString::JoinBy(Samples.SolidSamplesByHeight, TEXT(","),
                [](int32 Count) { return FString::FromInt(Count); }),
            Samples.PositionHash, Samples.DensityHash);
        Test.AddInfo(FString::Printf(
            TEXT("seed=%d samples=%d solids=%d blocking_solids=%d blocked_stations=%d floor_stations=%d missing_floors=%d max_missing_run=%d first_missing_floor=(station=%d,%.3f,%.3f,%.3f) width_floor_gaps=%d step_violations=%d max_step=%.3f floor_profile_violations=%d first_solid=(%.3f,%.3f,%.3f) density=%.6f"),
            Seed, Samples.Densities.Num(), Samples.SolidSamples,
            Samples.BlockingSolidSamples, Samples.BlockingSolidStations,
            Samples.FloorStations, Samples.MissingFloorStations,
            Samples.MaxMissingFloorRunStations, Samples.FirstMissingFloorStation,
            Samples.FirstMissingFloorPosition.X, Samples.FirstMissingFloorPosition.Y,
            Samples.FirstMissingFloorExpectedZ, Samples.WidthFloorGaps,
            Samples.StepViolations, Samples.MaxStepVoxels,
            Samples.FloorProfileViolations,
            Samples.FirstSolidPosition.X, Samples.FirstSolidPosition.Y,
            Samples.FirstSolidPosition.Z, Samples.FirstSolidDensity));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeDescentCorridorOwnerTest,
    "VoxelForge.Descent.OwnerPassageCorridor",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeDescentCorridorOwnerTest::RunTest(const FString& Parameters)
{
    FWalkCorridorSamples First;
    FString Error;
    if (!SampleOwnerSeed(/*Seed=*/0, First, Error))
    {
        AddError(Error);
        return false;
    }
    LogCorridorResult(*this, 0, First);

    FWalkCorridorSamples Repeat;
    if (!SampleOwnerSeed(/*Seed=*/0, Repeat, Error))
    {
        AddError(Error);
        return false;
    }
    LogCorridorResult(*this, 0, Repeat);
    const bool bSameSamples = First.Positions.Num() == Repeat.Positions.Num()
        && First.PositionHash == Repeat.PositionHash
        && First.DensityHash == Repeat.DensityHash
        && First.Positions.Num() == Repeat.Positions.Num()
        && FMemory::Memcmp(
            First.Densities.GetData(), Repeat.Densities.GetData(),
            First.Densities.Num() * sizeof(float)) == 0;
    TestTrue(FString::Printf(
        TEXT("Same owner seed produces identical corridor geometry and density samples (crc=%08X)"),
        First.DensityHash), bSameSamples);

    TestEqual(TEXT("Owner A-to-B corridor has no body obstruction above a legal step"),
        First.BlockingSolidSamples, 0);
    TestEqual(TEXT("Owner A-to-B corridor has support floor at every station"),
        First.MissingFloorStations, 0);
    TestEqual(TEXT("Owner A-to-B corridor has floor support across the full player width"),
        First.WidthFloorGaps, 0);
    TestEqual(TEXT("Owner A-to-B corridor has no step above 1.8 voxels"),
        First.StepViolations, 0);
    return First.BlockingSolidSamples == 0 && First.MissingFloorStations == 0
        && First.WidthFloorGaps == 0 && First.StepViolations == 0 && bSameSamples;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeDescentCorridorSeedSweepTest,
    "VoxelForge.Descent.OwnerSeedSweep",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeDescentCorridorSeedSweepTest::RunTest(const FString& Parameters)
{
    constexpr int32 SeedCount = 64;
    int32 SeedsWithSolids = 0;
    int32 SeedsWithBlockingSolids = 0;
    int32 SeedsWithFloorGaps = 0;
    int32 SeedsWithWidthGaps = 0;
    int32 SeedsWithLargeSteps = 0;
    int32 SeedsWithFloorProfileViolations = 0;
    int32 SeedsWithWalkFailures = 0;
    int64 TotalSolidSamples = 0;
    int64 TotalBlockingSolidSamples = 0;
    int64 TotalWidthFloorGaps = 0;
    int64 TotalSamples = 0;
    int32 FirstFailingSeed = INDEX_NONE;
    FWalkCorridorSamples Worst;
    FString WorstError;

    for (int32 Seed = 0; Seed < SeedCount; ++Seed)
    {
        FWalkCorridorSamples Samples;
        FString Error;
        if (!SampleOwnerSeed(Seed, Samples, Error))
        {
            AddError(Error);
            return false;
        }
        LogCorridorResult(*this, Seed, Samples);
        TotalSamples += Samples.Densities.Num();
        TotalSolidSamples += Samples.SolidSamples;
        TotalBlockingSolidSamples += Samples.BlockingSolidSamples;
        TotalWidthFloorGaps += Samples.WidthFloorGaps;
        if (Samples.SolidSamples > 0)
        {
            ++SeedsWithSolids;
        }
        if (Samples.BlockingSolidSamples > 0)
        {
            ++SeedsWithBlockingSolids;
            if (FirstFailingSeed == INDEX_NONE)
            {
                FirstFailingSeed = Seed;
                Worst = Samples;
                WorstError = FString::Printf(TEXT("seed %d"), Seed);
            }
            if (Samples.BlockingSolidSamples > Worst.BlockingSolidSamples)
            {
                Worst = Samples;
                WorstError = FString::Printf(TEXT("seed %d"), Seed);
            }
        }
        if (Samples.MissingFloorStations > 0)
        {
            ++SeedsWithFloorGaps;
        }
        if (Samples.WidthFloorGaps > 0)
        {
            ++SeedsWithWidthGaps;
        }
        if (Samples.StepViolations > 0)
        {
            ++SeedsWithLargeSteps;
        }
        if (Samples.FloorProfileViolations > 0)
        {
            ++SeedsWithFloorProfileViolations;
        }
        if (Samples.BlockingSolidSamples > 0 || Samples.MissingFloorStations > 0
            || Samples.WidthFloorGaps > 0 || Samples.StepViolations > 0)
        {
            ++SeedsWithWalkFailures;
        }
    }

    const double HitRate = 100.0 * static_cast<double>(SeedsWithWalkFailures)
        / static_cast<double>(SeedCount);
    UE_LOG(LogTemp, Display,
        TEXT("[VoxelForgeDescentCorridorSweep] seeds=%d walk_failure_seeds=%d hit_rate_pct=%.1f "
             "solid_seeds=%d blocking_seeds=%d floor_gap_seeds=%d width_gap_seeds=%d "
             "large_step_seeds=%d floor_profile_violation_seeds=%d "
             "solid_samples=%lld blocking_samples=%lld width_floor_gaps=%lld total_samples=%lld "
             "first_seed=%d worst_seed=%s worst_blockers=%d"),
        SeedCount, SeedsWithWalkFailures, HitRate, SeedsWithSolids, SeedsWithBlockingSolids,
        SeedsWithFloorGaps, SeedsWithWidthGaps, SeedsWithLargeSteps,
        SeedsWithFloorProfileViolations,
        static_cast<long long>(TotalSolidSamples),
        static_cast<long long>(TotalBlockingSolidSamples),
        static_cast<long long>(TotalWidthFloorGaps), static_cast<long long>(TotalSamples),
        FirstFailingSeed, WorstError.IsEmpty() ? TEXT("none") : *WorstError,
        Worst.BlockingSolidSamples);
    TestEqual(TEXT("All 64 owner seeds have a clear, walkable A-to-B body corridor"),
        SeedsWithWalkFailures, 0);
    return SeedsWithWalkFailures == 0;
}

#endif // WITH_DEV_AUTOMATION_TESTS
