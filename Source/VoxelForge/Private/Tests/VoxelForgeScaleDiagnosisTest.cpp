// Measurement-only scale diagnosis for the player-fit showcase.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"

#include "Components/CapsuleComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

#include "VoxelCaveMorphology.h"
#include "VoxelDensityOpStack.h"
#include "VoxelForgeTestFixture.h"
#include "VoxelStrateComposer.h"
#include "VoxelStrateMeasure.h"
#include "VoxelForgePlayerFitWindow.h"
#include "VoxelTypes.h"

namespace
{
    constexpr int32 DiagnosisSeed = 0;
    constexpr int32 MazeCandidateIndex = 50;
    // These are single-region seed-0 identities after the Maze default/range change updates the
    // corpus content hash. Keep the probes on real candidates rather than silently testing fixture
    // defaults when a historical index moves to another family.
    constexpr int32 TunnelCandidateIndex = 4;
    constexpr int32 ShaftCandidateIndex = 6;
    constexpr int32 RadialSampleMaxVoxels = 48;
    constexpr int32 FineDiagnosisMaxCells = 4000000;

    const TCHAR* VF_DiagnosisConnectivityName(EVoxelConnectivityResult Result)
    {
        switch (Result)
        {
        case EVoxelConnectivityResult::Connected:                   return TEXT("Connected");
        case EVoxelConnectivityResult::NotConnectedAtThisResolution: return TEXT("NotConnectedAtThisResolution");
        case EVoxelConnectivityResult::StartCellSolid:              return TEXT("StartCellSolid");
        case EVoxelConnectivityResult::GoalCellSolid:               return TEXT("GoalCellSolid");
        case EVoxelConnectivityResult::StartCellNotPlayerFit:       return TEXT("StartCellNotPlayerFit");
        case EVoxelConnectivityResult::GoalCellNotPlayerFit:        return TEXT("GoalCellNotPlayerFit");
        case EVoxelConnectivityResult::OutOfWindow:                 return TEXT("OutOfWindow");
        case EVoxelConnectivityResult::CoarseLiedBudgetExhausted:   return TEXT("CoarseLiedBudgetExhausted");
        }
        return TEXT("Unknown");
    }

    struct FDensityCrossing
    {
        bool bFound = false;
        float AxisDensity = 0.0f;
        float CrossingRadiusVoxels = -1.0f;
        int32 FirstSolidStep = -1;
        float FirstSolidDensity = 0.0f;
    };

    FDensityCrossing VF_SampleRadialRay(
        const FVector& AxisPoint,
        const FVector& UnitNormal,
        TFunctionRef<float(const FVector&)> SampleDensity,
        int32 MaxDistanceVoxels = RadialSampleMaxVoxels)
    {
        FDensityCrossing Result;
        float PreviousDensity = SampleDensity(AxisPoint);
        Result.AxisDensity = PreviousDensity;
        for (int32 Distance = 1; Distance <= MaxDistanceVoxels; ++Distance)
        {
            const float CurrentDensity = SampleDensity(
                AxisPoint + UnitNormal * static_cast<float>(Distance));
            // MC convention in this codebase: density >= 0 is air and density < 0 is solid.
            if (PreviousDensity >= 0.0f && CurrentDensity < 0.0f)
            {
                const float Denominator = PreviousDensity - CurrentDensity;
                const float Fraction = FMath::IsNearlyZero(Denominator)
                    ? 0.0f : FMath::Clamp(PreviousDensity / Denominator, 0.0f, 1.0f);
                Result.bFound = true;
                Result.CrossingRadiusVoxels = static_cast<float>(Distance - 1) + Fraction;
                Result.FirstSolidStep = Distance;
                Result.FirstSolidDensity = CurrentDensity;
                return Result;
            }
            PreviousDensity = CurrentDensity;
        }
        return Result;
    }

    struct FRadialSection
    {
        FVector AxisPoint = FVector::ZeroVector;
        FVector UnitNormal = FVector::ZeroVector;
        FDensityCrossing Positive;
        FDensityCrossing Negative;
        bool bValid = false;
    };

    FRadialSection VF_SampleSection(
        const FVector& AxisPoint,
        const FVector& UnitNormal,
        TFunctionRef<float(const FVector&)> SampleDensity)
    {
        FRadialSection Result;
        Result.AxisPoint = AxisPoint;
        Result.UnitNormal = UnitNormal.GetSafeNormal();
        if (Result.UnitNormal.IsNearlyZero())
        {
            return Result;
        }
        Result.Positive = VF_SampleRadialRay(AxisPoint, Result.UnitNormal, SampleDensity);
        Result.Negative = VF_SampleRadialRay(AxisPoint, -Result.UnitNormal, SampleDensity);
        Result.bValid = Result.Positive.bFound && Result.Negative.bFound;
        return Result;
    }

    float VF_SectionScore(const FRadialSection& Section)
    {
        return Section.bValid
            ? FMath::Min(Section.Positive.CrossingRadiusVoxels,
                         Section.Negative.CrossingRadiusVoxels)
            : -1.0f;
    }

    FVector VF_PerpendicularTo(const FVector& Tangent)
    {
        const FVector Basis = FMath::Abs(Tangent.Z) < 0.9f
            ? FVector::UpVector : FVector::RightVector;
        return FVector::CrossProduct(Tangent, Basis).GetSafeNormal();
    }

    void VF_SetCandidateOnSlot(
        VoxelForgeTest::FTestWorld& World,
        const FVoxelStrateComposerCandidate& Candidate,
        int32 SlotIndex)
    {
        UVoxelStrateDefinition* Definition = World.Definitions.IsValidIndex(SlotIndex)
            ? World.Definitions[SlotIndex].Get() : nullptr;
        if (Definition == nullptr)
        {
            return;
        }
        Definition->GeneratorType = Candidate.Archetype;
        Definition->GenerationParams = Candidate.ArchetypeParams.TunnelNetworkParams;
        Definition->SlabParams = Candidate.ArchetypeParams.SlabParams;
        Definition->MazeParams = Candidate.ArchetypeParams.MazeParams;
        Definition->SurfaceParams = Candidate.ArchetypeParams.SurfaceParams;
        Definition->VerticalShaftParams = Candidate.ArchetypeParams.VerticalShaftParams;
        Definition->FloatingIslandParams = Candidate.ArchetypeParams.FloatingIslandParams;
        Definition->bUseOperatorStack = true;
        Definition->TransitionType = EVoxelStrateTransition::Hard;
        World.Reinitialize();
    }

    int32 VF_MidChunkZ(const VoxelForgeTest::FTestWorld& World, int32 SlotIndex)
    {
        int32 TopVoxelZ = 0;
        int32 BottomVoxelZ = 0;
        if (!World.GetSlotVoxelZRange(SlotIndex, TopVoxelZ, BottomVoxelZ))
        {
            return 0;
        }
        return FMath::FloorToInt(
            0.5f * static_cast<float>(TopVoxelZ + 1 + BottomVoxelZ)
            / static_cast<float>(CHUNK_SIZE));
    }

    struct FCoreStackProbe
    {
        FVoxelOpStack Stack;

        void Prepare(int32 ChunkZ, float TopZ, float BottomZ,
                     float WorldRadiusVoxels, float EdgeSealThickness,
                     uint32 LayoutVersion)
        {
            FVoxelOpContext Context;
            Context.ChunkCoord = FIntVector(0, 0, ChunkZ);
            Context.Step = 1;
            Context.LayoutVersion = LayoutVersion;
            Context.WorldRadiusVoxels = WorldRadiusVoxels;
            Context.EdgeSealThickness = EdgeSealThickness;
            Context.StrateTopWorldZ = TopZ;
            Context.StrateBottomWorldZ = BottomZ;
            Stack.PrepareChunk(Context);
        }

        float Sample(const FVector& Point) const
        {
            return Stack.EvalMC(Point.X, Point.Y, Point.Z);
        }
    };

    bool VF_FindMazeAxis(
        const VoxelForgeTest::FTestWorld& World,
        const FMazeGenerationParams& Params,
        int32 SlotIndex,
        FRadialSection& OutFinalSection,
        FRadialSection& OutCoreSection,
        FString& OutDescription)
    {
        const float CellSize = FMath::Max(Params.CellSize, 1.0f);
        const uint32 Salt = static_cast<uint32>(World.Generator->Seed) ^ 0x4D617A65u;
        const int32 MidChunkZ = VF_MidChunkZ(World, SlotIndex);
        const int32 BaseCellZ = FMath::FloorToInt(
            (0.5f * (Params.StrateTopWorldZ + Params.StrateBottomWorldZ)) / CellSize);

        FCoreStackProbe Core;
        VoxelDensityOps::BuildMazeStack(
            Core.Stack, Params, World.Generator->Seed, World.Generator->OriginSpineRadius,
            World.StrateManager.Get(), false);
        Core.Prepare(MidChunkZ, Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
                     World.Generator->WorldRadiusVoxels, World.Generator->EdgeSealThickness,
                     World.StrateManager->GetLayoutVersion());

        auto FinalDensity = [&World](const FVector& Point)
        {
            return World.Generator->GetDensityAt(Point.X, Point.Y, Point.Z);
        };
        auto CoreDensity = [&Core](const FVector& Point)
        {
            return Core.Sample(Point);
        };

        float BestDistanceSq = FLT_MAX;
        for (int32 Ring = 1; Ring <= 12; ++Ring)
        {
            for (int32 NodeY = -Ring; NodeY <= Ring; ++NodeY)
            {
                for (int32 NodeX = -Ring; NodeX <= Ring; ++NodeX)
                {
                    if (FMath::Max(FMath::Abs(NodeX), FMath::Abs(NodeY)) != Ring)
                    {
                        continue;
                    }
                    for (int32 NodeZ = BaseCellZ - 1; NodeZ <= BaseCellZ + 1; ++NodeZ)
                    {
                        const FVector A(
                            (NodeX + 0.5f) * CellSize,
                            (NodeY + 0.5f) * CellSize,
                            (NodeZ + 0.5f) * CellSize);
                        const float DistanceSq = A.X * A.X + A.Y * A.Y;

                        struct FAxisSpec
                        {
                            VoxelMazeTopology::EAxis Axis;
                            FVector Delta;
                            const TCHAR* Name;
                        };
                        const FAxisSpec Axes[] = {
                            { VoxelMazeTopology::EAxis::X,
                              FVector(CellSize, 0.0f, 0.0f), TEXT("X") },
                            { VoxelMazeTopology::EAxis::Y,
                              FVector(0.0f, CellSize, 0.0f), TEXT("Y") },
                        };
                        for (const FAxisSpec& Axis : Axes)
                        {
                            if (!VoxelMazeTopology::IsOpenEdge(
                                    NodeX, NodeY, NodeZ, Axis.Axis, Salt,
                                    Params.BranchProbability, Params.Verticality))
                            {
                                continue;
                            }
                            const FVector B = A + Axis.Delta;
                            const FVector AxisPoint = 0.5f * (A + B);
                            if (AxisPoint.Z <= Params.StrateBottomWorldZ + Params.BoundarySealThickness + 8.0f
                                || AxisPoint.Z >= Params.StrateTopWorldZ - Params.BoundarySealThickness - 8.0f
                                || DistanceSq < FMath::Square(World.Generator->OriginSpineRadius + 24.0f))
                            {
                                continue;
                            }

                            const FVector Normal = VF_PerpendicularTo((B - A).GetSafeNormal());
                            const FRadialSection FinalSection = VF_SampleSection(
                                AxisPoint, Normal, FinalDensity);
                            const FRadialSection CoreSection = VF_SampleSection(
                                AxisPoint, Normal, CoreDensity);
                            if (!FinalSection.bValid || !CoreSection.bValid
                                || FinalSection.Positive.AxisDensity <= 0.0f)
                            {
                                continue;
                            }

                            if (DistanceSq >= BestDistanceSq)
                            {
                                continue;
                            }
                            BestDistanceSq = DistanceSq;
                            OutFinalSection = FinalSection;
                            OutCoreSection = CoreSection;
                            OutDescription = FString::Printf(
                                TEXT("Maze edge axis=%s node=(%d,%d,%d) endpointA=(%.2f,%.2f,%.2f) "
                                     "endpointB=(%.2f,%.2f,%.2f)"),
                                Axis.Name, NodeX, NodeY, NodeZ,
                                A.X, A.Y, A.Z, B.X, B.Y, B.Z);
                        }
                    }
                }
            }
            if (OutFinalSection.bValid)
            {
                return true;
            }
        }
        return false;
    }

    struct FTunnelAxisCandidate
    {
        bool bValid = false;
        float Score = -1.0f;
        float NominalRadius = 0.0f;
        FString Description;
        FRadialSection FinalSection;
        FRadialSection CoreSection;
    };

    void VF_ConsiderTunnelSegment(
        const VoxelForgeTest::FTestWorld& World,
        const FStrateGenerationParams& Params,
        const FCoreStackProbe& Core,
        const FVector& A,
        const FVector& B,
        float RadiusA,
        float RadiusB,
        const FString& SegmentName,
        FTunnelAxisCandidate& InOutBest)
    {
        const FVector Tangent = (B - A).GetSafeNormal();
        const float Length = FVector::Dist(A, B);
        if (Tangent.IsNearlyZero() || Length < 24.0f)
        {
            return;
        }

        const FVector Normal = VF_PerpendicularTo(Tangent);
        const FVector Binormal = FVector::CrossProduct(Tangent, Normal).GetSafeNormal();
        const float NominalRadius = FMath::Lerp(RadiusA, RadiusB, 0.5f);
        const int32 SearchExtent = 16;
        const int32 SearchStep = 4;
        const int32 MaxDistance = FMath::Clamp(
            FMath::CeilToInt(NominalRadius + FMath::Max(Params.SurfaceRoughness, 0.0f) + 20.0f),
            24, RadialSampleMaxVoxels);

        auto FinalDensity = [&World](const FVector& Point)
        {
            return World.Generator->GetDensityAt(Point.X, Point.Y, Point.Z);
        };
        auto CoreDensity = [&Core](const FVector& Point)
        {
            return Core.Sample(Point);
        };

        for (int32 OffsetB = -SearchExtent; OffsetB <= SearchExtent; OffsetB += SearchStep)
        {
            for (int32 OffsetN = -SearchExtent; OffsetN <= SearchExtent; OffsetN += SearchStep)
            {
                const FVector AxisPoint = 0.5f * (A + B)
                    + Binormal * static_cast<float>(OffsetB)
                    + Normal * static_cast<float>(OffsetN);
                if (AxisPoint.Z <= Params.StrateBottomWorldZ + Params.BoundarySealThickness + 8.0f
                    || AxisPoint.Z >= Params.StrateTopWorldZ - Params.BoundarySealThickness - 8.0f
                    || FVector2D(AxisPoint.X, AxisPoint.Y).SizeSquared()
                        < FMath::Square(World.Generator->OriginSpineRadius + 24.0f))
                {
                    continue;
                }

                const FRadialSection FinalSection = VF_SampleSection(
                    AxisPoint, Normal, FinalDensity);
                if (!FinalSection.bValid)
                {
                    continue;
                }
                const FRadialSection CoreSection = VF_SampleSection(
                    AxisPoint, Normal, CoreDensity);
                if (!CoreSection.bValid)
                {
                    continue;
                }

                const float Score = VF_SectionScore(FinalSection);
                if (Score <= InOutBest.Score)
                {
                    continue;
                }
                InOutBest.bValid = true;
                InOutBest.Score = Score;
                InOutBest.NominalRadius = NominalRadius;
                InOutBest.Description = FString::Printf(
                    TEXT("%s segment A=(%.2f,%.2f,%.2f) B=(%.2f,%.2f,%.2f) "
                         "axisOffset=(%.2f,%.2f,%.2f)"),
                    *SegmentName, A.X, A.Y, A.Z, B.X, B.Y, B.Z,
                    AxisPoint.X - 0.5f * (A.X + B.X),
                    AxisPoint.Y - 0.5f * (A.Y + B.Y),
                    AxisPoint.Z - 0.5f * (A.Z + B.Z));
                InOutBest.FinalSection = FinalSection;
                InOutBest.CoreSection = CoreSection;
            }
        }
    }

    bool VF_FindTunnelAxis(
        const VoxelForgeTest::FTestWorld& World,
        const FStrateGenerationParams& Params,
        int32 SlotIndex,
        FTunnelAxisCandidate& OutBest)
    {
        const int32 MidChunkZ = VF_MidChunkZ(World, SlotIndex);
        FCoreStackProbe Core;
        VoxelDensityOps::BuildTunnelNetworkStack(
            Core.Stack, Params, World.Generator->Seed, World.Generator->OriginSpineRadius,
            World.StrateManager.Get(), false);
        Core.Prepare(MidChunkZ, Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
                     World.Generator->WorldRadiusVoxels, World.Generator->EdgeSealThickness,
                     World.StrateManager->GetLayoutVersion());

        FChunkSDFCache Cache;
        const float SearchExtent = FMath::Max(
            512.0f, FMath::Max(Params.RoomSpacing * 4.0f, Params.MaxTunnelLength * 2.0f));
        VoxelCaveMorphology::BuildChunkCache(
            Cache, -SearchExtent, -SearchExtent, SearchExtent, SearchExtent,
            Params, static_cast<uint32>(World.Generator->Seed), SlotIndex, nullptr);

        for (int32 TunnelIndex = 0; TunnelIndex < Cache.Tunnels.Num(); ++TunnelIndex)
        {
            const FCachedTunnel& Tunnel = Cache.Tunnels[TunnelIndex];
            if (Tunnel.bHasMidpoint)
            {
                VF_ConsiderTunnelSegment(
                    World, Params, Core, Tunnel.EndpointA, Tunnel.Midpoint,
                    Tunnel.RadiusA, Tunnel.RadiusMid,
                    FString::Printf(TEXT("tunnel[%d].A-mid"), TunnelIndex), OutBest);
                VF_ConsiderTunnelSegment(
                    World, Params, Core, Tunnel.Midpoint, Tunnel.EndpointB,
                    Tunnel.RadiusMid, Tunnel.RadiusB,
                    FString::Printf(TEXT("tunnel[%d].mid-B"), TunnelIndex), OutBest);
            }
            else
            {
                VF_ConsiderTunnelSegment(
                    World, Params, Core, Tunnel.EndpointA, Tunnel.EndpointB,
                    Tunnel.RadiusA, Tunnel.RadiusB,
                    FString::Printf(TEXT("tunnel[%d]"), TunnelIndex), OutBest);
            }
        }
        return OutBest.bValid;
    }

    bool VF_FindShaftAxis(
        const VoxelForgeTest::FTestWorld& World,
        const FVerticalShaftParams& Params,
        int32 SlotIndex,
        FRadialSection& OutFinalSection,
        FRadialSection& OutCoreSection,
        float& OutNominalRadius,
        FString& OutDescription)
    {
        const float Spacing = FMath::Max(Params.ShaftSpacing, 1.0f);
        const uint32 Salt = static_cast<uint32>(World.Generator->Seed) ^ 0x53686674u;
        const int32 MidChunkZ = VF_MidChunkZ(World, SlotIndex);
        FCoreStackProbe Core;
        VoxelDensityOps::BuildVerticalShaftStack(
            Core.Stack, Params, World.Generator->Seed, World.Generator->OriginSpineRadius,
            World.StrateManager.Get(), false);
        Core.Prepare(MidChunkZ, Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
                     World.Generator->WorldRadiusVoxels, World.Generator->EdgeSealThickness,
                     World.StrateManager->GetLayoutVersion());

        auto FinalDensity = [&World](const FVector& Point)
        {
            return World.Generator->GetDensityAt(Point.X, Point.Y, Point.Z);
        };
        auto CoreDensity = [&Core](const FVector& Point)
        {
            return Core.Sample(Point);
        };

        const float InnerBottom = Params.StrateBottomWorldZ + Params.BoundarySealThickness + 4.0f;
        const float InnerTop = Params.StrateTopWorldZ - Params.BoundarySealThickness - 4.0f;
        const float Period = FMath::Max(Params.LedgeSpacing, 32.0f);
        for (int32 Ring = 1; Ring <= 32; ++Ring)
        {
            for (int32 CellY = -Ring; CellY <= Ring; ++CellY)
            {
                for (int32 CellX = -Ring; CellX <= Ring; ++CellX)
                {
                    if (FMath::Max(FMath::Abs(CellX), FMath::Abs(CellY)) != Ring)
                    {
                        continue;
                    }
                    const uint32 CellHash = VoxelHash::Cell(CellX, CellY, Salt);
                    if (VoxelHash::ToFloat01(CellHash) > Params.ShaftDensity)
                    {
                        continue;
                    }
                    const float JitterX = VoxelHash::ToFloat01(
                        VoxelHash::Mix(CellHash ^ 0x12345678u));
                    const float JitterY = VoxelHash::ToFloat01(
                        VoxelHash::Mix(CellHash ^ 0x9ABCDEF0u));
                    const float ShaftX = (CellX + 0.15f + JitterX * 0.7f) * Spacing;
                    const float ShaftY = (CellY + 0.15f + JitterY * 0.7f) * Spacing;
                    const float Radius = FMath::Lerp(
                        Params.ShaftMinRadius, Params.ShaftMaxRadius,
                        VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0xBEEFu)));
                    if (FMath::Square(ShaftX) + FMath::Square(ShaftY)
                        < FMath::Square(World.Generator->OriginSpineRadius + 24.0f))
                    {
                        continue;
                    }

                    for (int32 Level = 0; Level < 16; ++Level)
                    {
                        const float Z = InnerBottom + (static_cast<float>(Level) + 0.5f) * Period;
                        if (Z >= InnerTop)
                        {
                            break;
                        }
                        const FVector AxisPoint(ShaftX, ShaftY, Z);
                        const FRadialSection FinalSection = VF_SampleSection(
                            AxisPoint, FVector(1.0f, 0.0f, 0.0f), FinalDensity);
                        const FRadialSection CoreSection = VF_SampleSection(
                            AxisPoint, FVector(1.0f, 0.0f, 0.0f), CoreDensity);
                        if (!FinalSection.bValid || !CoreSection.bValid)
                        {
                            continue;
                        }
                        OutFinalSection = FinalSection;
                        OutCoreSection = CoreSection;
                        OutNominalRadius = Radius;
                        OutDescription = FString::Printf(
                            TEXT("shaft cell=(%d,%d) center=(%.2f,%.2f) z=%.2f"),
                            CellX, CellY, ShaftX, ShaftY, Z);
                        return true;
                    }
                }
            }
        }
        return false;
    }

    class FEmptyRoomSampler final : public IVoxelStrateDensitySampler
    {
    public:
        float SampleDensity(float, float, float WorldZ) const override
        {
            // A 20 m square room: z=0.5 is the one-cell solid floor and every sampled
            // cell above it is empty air. This is deliberately a control for the stencil,
            // not a generation change.
            return WorldZ < 1.0f ? -1.0f : 1.0f;
        }
    };

    class FGeneratorSampler final : public IVoxelStrateDensitySampler
    {
    public:
        explicit FGeneratorSampler(const UVoxelGenerator& InGenerator)
            : Generator(InGenerator) {}

        float SampleDensity(float WorldX, float WorldY, float WorldZ) const override
        {
            return Generator.GetDensityAt(WorldX, WorldY, WorldZ);
        }

    private:
        const UVoxelGenerator& Generator;
    };

    class FVerticalWallSampler final : public IVoxelStrateDensitySampler
    {
    public:
        float SampleDensity(float WorldX, float, float) const override
        {
            // A wall that spans the whole measured height has no lower supporting surface.
            // It catches a stencil that treats a nearby solid column as a floor pixel.
            return FMath::Abs(WorldX) < 1.0f ? -1.0f : 1.0f;
        }
    };

    class FIsolatedLedgeSampler final : public IVoxelStrateDensitySampler
    {
    public:
        float SampleDensity(float WorldX, float WorldY, float WorldZ) const override
        {
            // With one-voxel sampling and bounds [-8,8), this is exactly one solid floor cell
            // at the (0.5,0.5,0.5) sample. It must fail the 4-of-5 support patch.
            const bool bSingleCell = WorldZ < 1.0f
                && FMath::Abs(WorldX) < 1.0f
                && FMath::Abs(WorldY) < 1.0f;
            return bSingleCell ? -1.0f : 1.0f;
        }
    };

    class FOverhangSampler final : public IVoxelStrateDensitySampler
    {
    public:
        float SampleDensity(float, float, float WorldZ) const override
        {
            // Air below an overhang/ceiling, with solid continuing above the measured window.
            // A downward-only support search must not use that ceiling as a floor.
            return WorldZ >= 4.0f ? -1.0f : 1.0f;
        }
    };

    class FSteepRampSampler final : public IVoxelStrateDensitySampler
    {
    public:
        float SampleDensity(float WorldX, float, float WorldZ) const override
        {
            // One voxel of rise per one voxel of run is 45 degrees. The project CDO's 44.8
            // degree limit must reject the support patch instead of averaging this into floor.
            const float FloorHeight = 8.0f + WorldX;
            return WorldZ < FloorHeight ? -1.0f : 1.0f;
        }
    };

    bool VF_ApplyProjectCharacterFitDefaults(
        FAutomationTestBase& Test,
        FVoxelStrateMeasureSettings& InOutSettings)
    {
        constexpr const TCHAR* CharacterCDOPath =
            TEXT("/Game/FirstPerson/Blueprints/BP_FirstPersonCharacter.BP_FirstPersonCharacter_C");
        UClass* CharacterClass = LoadObject<UClass>(nullptr, CharacterCDOPath);
        if (CharacterClass == nullptr || !CharacterClass->IsChildOf(ACharacter::StaticClass()))
        {
            Test.AddError(FString::Printf(
                TEXT("Could not load the project character CDO at %s."), CharacterCDOPath));
            return false;
        }

        const ACharacter* CharacterCDO = CharacterClass->GetDefaultObject<ACharacter>();
        const UCapsuleComponent* Capsule = CharacterCDO != nullptr
            ? CharacterCDO->GetCapsuleComponent() : nullptr;
        const UCharacterMovementComponent* Movement = CharacterCDO != nullptr
            ? CharacterCDO->GetCharacterMovement() : nullptr;
        if (Capsule == nullptr || Movement == nullptr)
        {
            Test.AddError(TEXT("The project character CDO has no capsule or movement component."));
            return false;
        }

        const float RadiusCentimeters = Capsule->GetUnscaledCapsuleRadius();
        const float HalfHeightCentimeters = Capsule->GetUnscaledCapsuleHalfHeight();
        const float MaxStepHeightMeters = Movement->MaxStepHeight / 100.0f;
        const float WalkableFloorAngleDegrees = Movement->GetWalkableFloorAngle();
        if (!FMath::IsFinite(RadiusCentimeters) || RadiusCentimeters <= 0.0f
            || !FMath::IsFinite(HalfHeightCentimeters) || HalfHeightCentimeters <= 0.0f
            || !FMath::IsFinite(MaxStepHeightMeters) || MaxStepHeightMeters < 0.0f
            || !FMath::IsFinite(WalkableFloorAngleDegrees)
            || WalkableFloorAngleDegrees < 0.0f || WalkableFloorAngleDegrees > 90.0f)
        {
            Test.AddError(TEXT("The project character CDO has invalid fit parameters."));
            return false;
        }

        // Keep the existing 34 cm / 88 cm metric capsule fixed for before/after comparability.
        // The task-specific movement limits below are read from the project character CDO.
        Test.AddInfo(FString::Printf(
            TEXT("PLAYER_CHARACTER capsule CDO reports radius=%.1f cm, half_height=%.1f cm; "
                 "measurement dimensions remain radius=%.1f cm, half_height=%.1f cm "
                 "to isolate the stencil change"),
            RadiusCentimeters,
            HalfHeightCentimeters,
            FVoxelPlayerCapsuleConstants::RadiusCentimeters,
            FVoxelPlayerCapsuleConstants::HalfHeightCentimeters));
        InOutSettings.PlayerMaxStepHeightMeters = MaxStepHeightMeters;
        InOutSettings.PlayerWalkableFloorAngleDegrees = WalkableFloorAngleDegrees;
        return true;
    }

    int32 VF_CountSupportFootprintColumns(float RadiusVoxels)
    {
        const int32 MaxOffset = FMath::CeilToInt(RadiusVoxels);
        int32 Count = 0;
        for (int32 Y = -MaxOffset; Y <= MaxOffset; ++Y)
        {
            for (int32 X = -MaxOffset; X <= MaxOffset; ++X)
            {
                if (static_cast<float>(X * X + Y * Y)
                    <= RadiusVoxels * RadiusVoxels + KINDA_SMALL_NUMBER)
                {
                    ++Count;
                }
            }
        }
        return Count;
    }

    float VF_BoundarySealForCandidate(const FVoxelStrateComposerCandidate& Candidate)
    {
        switch (Candidate.Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return Candidate.ArchetypeParams.TunnelNetworkParams.BoundarySealThickness;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            return Candidate.ArchetypeParams.SlabParams.BoundarySealThickness;
        case ECaveGeneratorType::Maze:
            return Candidate.ArchetypeParams.MazeParams.BoundarySealThickness;
        case ECaveGeneratorType::SurfaceWorld:
            return Candidate.ArchetypeParams.SurfaceParams.BoundarySealThickness;
        case ECaveGeneratorType::VerticalShafts:
            return Candidate.ArchetypeParams.VerticalShaftParams.BoundarySealThickness;
        case ECaveGeneratorType::FloatingIslands:
            return Candidate.ArchetypeParams.FloatingIslandParams.BoundarySealThickness;
        default:
            return 0.0f;
        }
    }

    int32 VF_MaxCenteredRadiusForCap(
        int32 TopVoxelZ,
        int32 BottomVoxelZ,
        float BoundarySealThickness,
        const FVoxelStrateMeasureSettings& Settings)
    {
        const int64 StrateHeight = static_cast<int64>(TopVoxelZ) + 1
            - static_cast<int64>(BottomVoxelZ);
        const int64 MaxMargin64 = FMath::Max<int64>(1, StrateHeight / 4);
        const int32 MaxMargin = static_cast<int32>(FMath::Min<int64>(
            MaxMargin64, static_cast<int64>(INT32_MAX)));
        int32 RequestedMargin = Settings.InteriorMarginVoxels;
        if (RequestedMargin < 0)
        {
            RequestedMargin = FMath::CeilToInt(2.0f * BoundarySealThickness);
        }
        const int32 ResolvedMargin = FMath::Clamp(RequestedMargin, 0, MaxMargin);
        const int64 InteriorHeight = StrateHeight - 2 * static_cast<int64>(ResolvedMargin);
        if (InteriorHeight <= 0 || Settings.SampleStep <= 0 || Settings.MaxCells <= 0)
        {
            return 0;
        }

        const int64 NumZ = (InteriorHeight + Settings.SampleStep - 1)
            / static_cast<int64>(Settings.SampleStep);
        const int64 MaxAxisCells = FMath::Min<int64>(
            static_cast<int64>(INT32_MAX),
            static_cast<int64>(FMath::Sqrt(
                static_cast<double>(Settings.MaxCells / FMath::Max<int64>(NumZ, 1)))));
        int32 Radius = static_cast<int32>(FMath::Max<int64>(
            1, (MaxAxisCells * static_cast<int64>(Settings.SampleStep)) / 2));
        auto CellCountForRadius = [Settings, NumZ](int32 CandidateRadius) -> int64
        {
            const int64 NumXY = (2 * static_cast<int64>(CandidateRadius)
                                 + Settings.SampleStep - 1)
                / static_cast<int64>(Settings.SampleStep);
            if (NumXY <= 0 || NumXY > INT64_MAX / NumXY
                || NumXY * NumXY > INT64_MAX / FMath::Max<int64>(NumZ, 1))
            {
                return INT64_MAX;
            }
            return NumXY * NumXY * NumZ;
        };
        while (Radius > 1 && CellCountForRadius(Radius) > Settings.MaxCells)
        {
            --Radius;
        }
        while (Radius < INT32_MAX
            && CellCountForRadius(Radius + 1) <= Settings.MaxCells)
        {
            ++Radius;
        }
        return Radius;
    }

    void VF_ReportPlayerFitRow(
        FAutomationTestBase& Test,
        const TCHAR* Label,
        const FString& WindowLabel,
        int32 SampleStep,
        const FVoxelStrateMetrics& Metrics,
        const FVoxelConnectivityDiagnostics& Diagnostics)
    {
        if (!Metrics.bValid || !Metrics.bPlayerFitResolved)
        {
            const FString& Refusal = !Metrics.RefusalReason.IsEmpty()
                ? Metrics.RefusalReason : Metrics.PlayerFitRefusalReason;
            Test.AddInfo(FString::Printf(
                TEXT("PLAYER_FIT step=%d window=%s %s: REFUSED reason=%s"),
                SampleStep,
                *WindowLabel,
                Label,
                Refusal.IsEmpty() ? TEXT("player-fit metrics unresolved") : *Refusal));
            return;
        }
        Test.AddInfo(FString::Printf(
            TEXT("PLAYER_FIT step=%d window=%s bounds=X[%.1f,%.1f) Y[%.1f,%.1f) "
                 "Z[%d,%d) grid=%dx%dx%d sampled=%lld %s: fit_cells=%lld components=%d "
                 "largest_component_cells=%lld arrival_component_cells=%lld "
                 "departure_component_cells=%lld mouth_component_gap_voxels=%.3f "
                 "fit_fraction=%.9f traversable_share=%.9f law=%s"),
            SampleStep,
            *WindowLabel,
            Metrics.SampledMinX,
            Metrics.SampledMaxX,
            Metrics.SampledMinY,
            Metrics.SampledMaxY,
            Metrics.SampledMinZ,
            Metrics.SampledMaxZ,
            Metrics.SampledNumX,
            Metrics.SampledNumY,
            Metrics.SampledNumZ,
            static_cast<long long>(Metrics.NumSampled),
            Label,
            static_cast<long long>(Metrics.NumPlayerFitCells),
            Metrics.NumTraversableComponents,
            static_cast<long long>(Metrics.LargestTraversableComponentCells),
            static_cast<long long>(Diagnostics.StartComponentCells),
            static_cast<long long>(Diagnostics.GoalComponentCells),
            Diagnostics.StartToGoalComponentDistanceCells,
            Metrics.PlayerFitFraction,
            Metrics.TraversableComponentShare,
            VF_DiagnosisConnectivityName(Diagnostics.Result)));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeScaleDiagnosisTest,
    "VoxelForge.Composer.ScaleDiagnosis",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeScaleDiagnosisTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;
    (void)Parameters;

    UVoxelSettings* AuthoredSettings = LoadObject<UVoxelSettings>(
        nullptr, TEXT("/Game/VoxelForge/DA_Settings.DA_Settings"));
    if (AuthoredSettings == nullptr)
    {
        AddError(TEXT("Could not load /Game/VoxelForge/DA_Settings.DA_Settings."));
        return false;
    }

    FVoxelStrateCorpus Corpus;
    FString CorpusReport;
    const bool bCorpusLoaded = Corpus.LoadFromAssetRegistry(CorpusReport);
    AddInfo(CorpusReport);
    TestTrue(TEXT("scale-diagnosis corpus is usable"), bCorpusLoaded && Corpus.IsValid());
    if (!bCorpusLoaded || !Corpus.IsValid())
    {
        return false;
    }

    FTestWorld World;
    World.Build(DiagnosisSeed, 2, true, 8);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }
    TestTrue(TEXT("scale diagnosis keeps WorldRadiusVoxels at zero"),
             World.Settings->WorldRadiusVoxels == 0.0f
                 && World.Generator->WorldRadiusVoxels == 0.0f);
    TestFalse(TEXT("scale diagnosis keeps lateral regions gated off"),
              VF_LateralRegionsAreShippable());

    const FVoxelStrateComposerCandidate MazeCandidate = VF_RollStrateCandidate(
        Corpus, DiagnosisSeed, MazeCandidateIndex, false);
    const FVoxelStrateComposerCandidate TunnelCandidate = VF_RollStrateCandidate(
        Corpus, DiagnosisSeed, TunnelCandidateIndex, false);
    const FVoxelStrateComposerCandidate ShaftCandidate = VF_RollStrateCandidate(
        Corpus, DiagnosisSeed, ShaftCandidateIndex, false);
    TestTrue(TEXT("seed 0 index 50 is a valid Maze candidate"),
             MazeCandidate.bValid && MazeCandidate.Archetype == ECaveGeneratorType::Maze);
    TestTrue(TEXT("seed 0 index 61 is a valid TunnelNetwork candidate"),
             TunnelCandidate.bValid && TunnelCandidate.Archetype == ECaveGeneratorType::TunnelNetwork);
    TestTrue(TEXT("seed 0 index 34 is a valid VerticalShafts candidate"),
             ShaftCandidate.bValid && ShaftCandidate.Archetype == ECaveGeneratorType::VerticalShafts);

    // H1 — direct GetDensityAt radial crossings. The Maze is the fourth one-based layout slot
    // (FTestWorld uses zero-based SlotMaze == 3), matching the requested "slot 4".
    const int32 MazeSlot = FTestWorld::SlotMaze;
    VF_SetCandidateOnSlot(World, MazeCandidate, MazeSlot);
    int32 MazeTop = 0;
    int32 MazeBottom = 0;
    TestTrue(TEXT("Maze diagnosis slot has a Z range"),
             World.GetSlotVoxelZRange(MazeSlot, MazeTop, MazeBottom));
    const FMazeGenerationParams MazeParams = World.StrateManager->GetMazeParamsForChunk(
        FIntVector(0, 0, VF_MidChunkZ(World, MazeSlot)));
    FRadialSection MazeFinal;
    FRadialSection MazeCore;
    FString MazeAxisDescription;
    const bool bHaveMazeAxis = VF_FindMazeAxis(
        World, MazeParams, MazeSlot, MazeFinal, MazeCore, MazeAxisDescription);
    TestTrue(TEXT("Maze diagnosis found an open corridor axis"), bHaveMazeAxis);
    if (bHaveMazeAxis)
    {
        AddInfo(FString::Printf(
            TEXT("BORE maze seed=%d candidate=%d slot=%d(one-based 4) %s: "
                 "parameter_radius=%.3f vox (%.3f m), parameter_bore=%.3f m; "
                 "GetDensityAt +ray crossing=%.3f vox (%.3f m), first_solid_step=%d "
                 "core_without_structural_posts +ray=%.3f vox (%.3f m); "
                 "-ray final=%.3f vox (%.3f m), -ray core=%.3f vox (%.3f m)"),
            DiagnosisSeed, MazeCandidateIndex, MazeSlot, *MazeAxisDescription,
            MazeParams.CorridorRadius, MazeParams.CorridorRadius * 0.25f,
            MazeParams.CorridorRadius * 2.0f * 0.25f,
            MazeFinal.Positive.CrossingRadiusVoxels,
            MazeFinal.Positive.CrossingRadiusVoxels * 0.25f,
            MazeFinal.Positive.FirstSolidStep,
            MazeCore.Positive.CrossingRadiusVoxels,
            MazeCore.Positive.CrossingRadiusVoxels * 0.25f,
            MazeFinal.Negative.CrossingRadiusVoxels,
            MazeFinal.Negative.CrossingRadiusVoxels * 0.25f,
            MazeCore.Negative.CrossingRadiusVoxels,
            MazeCore.Negative.CrossingRadiusVoxels * 0.25f));
    }

    // TunnelNetwork — use the current fixed showcase roll and recover a real cached tunnel
    // segment, then sample the final GetDensityAt field at a locally centred radial section.
    const int32 TunnelSlot = FTestWorld::SlotTunnelNetwork;
    VF_SetCandidateOnSlot(World, TunnelCandidate, TunnelSlot);
    const FStrateGenerationParams TunnelParams = World.StrateManager->GetGenerationParams(
        FIntVector(0, 0, VF_MidChunkZ(World, TunnelSlot)));
    FTunnelAxisCandidate TunnelAxis;
    const bool bHaveTunnelAxis = VF_FindTunnelAxis(
        World, TunnelParams, TunnelSlot, TunnelAxis);
    TestTrue(TEXT("TunnelNetwork diagnosis found a tunnel axis"), bHaveTunnelAxis);
    if (bHaveTunnelAxis)
    {
        AddInfo(FString::Printf(
            TEXT("BORE tunnel seed=%d candidate=%d %s: parameter_radius_range=%.3f..%.3f vox "
                 "(%.3f..%.3f m), nominal_radius_at_probe=%.3f vox (%.3f m); "
                 "GetDensityAt +ray crossing=%.3f vox (%.3f m), -ray=%.3f vox (%.3f m); "
                 "core_without_structural_posts +ray=%.3f vox (%.3f m), -ray=%.3f vox (%.3f m)"),
            DiagnosisSeed, TunnelCandidateIndex, *TunnelAxis.Description,
            TunnelParams.TunnelMinRadius, TunnelParams.TunnelMaxRadius,
            TunnelParams.TunnelMinRadius * 0.25f, TunnelParams.TunnelMaxRadius * 0.25f,
            TunnelAxis.NominalRadius, TunnelAxis.NominalRadius * 0.25f,
            TunnelAxis.FinalSection.Positive.CrossingRadiusVoxels,
            TunnelAxis.FinalSection.Positive.CrossingRadiusVoxels * 0.25f,
            TunnelAxis.FinalSection.Negative.CrossingRadiusVoxels,
            TunnelAxis.FinalSection.Negative.CrossingRadiusVoxels * 0.25f,
            TunnelAxis.CoreSection.Positive.CrossingRadiusVoxels,
            TunnelAxis.CoreSection.Positive.CrossingRadiusVoxels * 0.25f,
            TunnelAxis.CoreSection.Negative.CrossingRadiusVoxels,
            TunnelAxis.CoreSection.Negative.CrossingRadiusVoxels * 0.25f));
    }

    // VerticalShafts — recover the deterministic shaft centre from the same hash contract as
    // production, then sample horizontally at a non-ledge height.
    const int32 ShaftSlot = FTestWorld::SlotVerticalShafts;
    VF_SetCandidateOnSlot(World, ShaftCandidate, ShaftSlot);
    const FVerticalShaftParams ShaftParams = World.StrateManager->GetVerticalShaftParamsForChunk(
        FIntVector(0, 0, VF_MidChunkZ(World, ShaftSlot)));
    FRadialSection ShaftFinal;
    FRadialSection ShaftCore;
    float ShaftNominalRadius = 0.0f;
    FString ShaftAxisDescription;
    const bool bHaveShaftAxis = VF_FindShaftAxis(
        World, ShaftParams, ShaftSlot, ShaftFinal, ShaftCore,
        ShaftNominalRadius, ShaftAxisDescription);
    TestTrue(TEXT("VerticalShafts diagnosis found a shaft axis"), bHaveShaftAxis);
    if (bHaveShaftAxis)
    {
        AddInfo(FString::Printf(
            TEXT("BORE shaft seed=%d candidate=%d %s: parameter_radius_range=%.3f..%.3f vox "
                 "(%.3f..%.3f m), nominal_radius_at_probe=%.3f vox (%.3f m); "
                 "GetDensityAt +ray crossing=%.3f vox (%.3f m), -ray=%.3f vox (%.3f m); "
                 "core_without_structural_posts +ray=%.3f vox (%.3f m), -ray=%.3f vox (%.3f m)"),
            DiagnosisSeed, ShaftCandidateIndex, *ShaftAxisDescription,
            ShaftParams.ShaftMinRadius, ShaftParams.ShaftMaxRadius,
            ShaftParams.ShaftMinRadius * 0.25f, ShaftParams.ShaftMaxRadius * 0.25f,
            ShaftNominalRadius, ShaftNominalRadius * 0.25f,
            ShaftFinal.Positive.CrossingRadiusVoxels,
            ShaftFinal.Positive.CrossingRadiusVoxels * 0.25f,
            ShaftFinal.Negative.CrossingRadiusVoxels,
            ShaftFinal.Negative.CrossingRadiusVoxels * 0.25f,
            ShaftCore.Positive.CrossingRadiusVoxels,
            ShaftCore.Positive.CrossingRadiusVoxels * 0.25f,
            ShaftCore.Negative.CrossingRadiusVoxels,
            ShaftCore.Negative.CrossingRadiusVoxels * 0.25f));
    }

    FVoxelStrateMeasureSettings CharacterFitSettings;
    if (!VF_ApplyProjectCharacterFitDefaults(*this, CharacterFitSettings))
    {
        return false;
    }

    const float MinimumWalkableNormalZ = FMath::Cos(FMath::DegreesToRadians(
        CharacterFitSettings.PlayerWalkableFloorAngleDegrees));
    const int32 SupportFootprintColumns = VF_CountSupportFootprintColumns(
        CharacterFitSettings.PlayerCapsuleRadiusVoxels);
    const int32 RequiredSupportColumns = FMath::Clamp(
        FMath::CeilToInt(CharacterFitSettings.PlayerSupportPatchMinCoverageFraction
                         * static_cast<float>(SupportFootprintColumns)),
        1, SupportFootprintColumns);
    const int32 MaxDownwardSearchCells = FMath::CeilToInt(
        CharacterFitSettings.PlayerMaxStepHeightMeters
        / FVoxelPlayerCapsuleConstants::VoxelSizeMeters);
    AddInfo(FString::Printf(
        TEXT("PLAYER_CHARACTER movement source=%s: MaxStepHeight=%.3f m "
             "(%.3f vox; search<=%d cells), WalkableFloorAngle=%.1f deg "
             "(normal_z>=%.6f); support_patch=%.1f%% requires %d/%d integer footprint columns "
             "and always requires the centre"),
        TEXT("/Game/FirstPerson/Blueprints/BP_FirstPersonCharacter CDO"),
        CharacterFitSettings.PlayerMaxStepHeightMeters,
        CharacterFitSettings.PlayerMaxStepHeightMeters
            / FVoxelPlayerCapsuleConstants::VoxelSizeMeters,
        MaxDownwardSearchCells,
        CharacterFitSettings.PlayerWalkableFloorAngleDegrees,
        MinimumWalkableNormalZ,
        CharacterFitSettings.PlayerSupportPatchMinCoverageFraction * 100.0f,
        RequiredSupportColumns,
        SupportFootprintColumns));

    // H2 — a known-open control with a solid floor. This must produce non-zero fit cells.
    FEmptyRoomSampler EmptyRoom;
    FVoxelStrateMeasureSettings ControlSettings = CharacterFitSettings;
    ControlSettings.SampleStep = 1;
    ControlSettings.RadiusInVoxels = 40; // 80 voxels = 20 m across.
    ControlSettings.CenterXY = FVector2D::ZeroVector;
    ControlSettings.MaxCells = 300000;
    ControlSettings.HeadroomCells = 2;
    ControlSettings.InteriorMarginVoxels = 0;
    FVoxelStrateMetrics ControlMetrics = VF_MeasureStrateWithSampler(
        EmptyRoom, 0, 32, 0.0f, ControlSettings, nullptr);
    TestTrue(TEXT("empty-room player-fit control resolved"),
             ControlMetrics.bValid && ControlMetrics.bPlayerFitResolved);
    AddInfo(FString::Printf(
        TEXT("CONTROL empty_room=20m_square_with_one_cell_floor step=1: "
             "sampled=%lld air=%lld fit_cells=%lld fit_fraction=%.9f "
             "components=%d largest_component_cells=%lld traversable_share=%.9f"),
        static_cast<long long>(ControlMetrics.NumSampled),
        static_cast<long long>(ControlMetrics.NumAir),
        static_cast<long long>(ControlMetrics.NumPlayerFitCells),
        ControlMetrics.PlayerFitFraction,
        ControlMetrics.NumTraversableComponents,
        static_cast<long long>(ControlMetrics.LargestTraversableComponentCells),
        ControlMetrics.TraversableComponentShare));

    FVoxelStrateMeasureSettings CoarseControlSettings = ControlSettings;
    CoarseControlSettings.SampleStep = 4;
    const FVoxelStrateMetrics CoarseControlMetrics = VF_MeasureStrateWithSampler(
        EmptyRoom, 0, 32, 0.0f, CoarseControlSettings, nullptr);
    const bool bCoarseRefused = CoarseControlMetrics.bValid
        && !CoarseControlMetrics.bPlayerFitResolved
        && CoarseControlMetrics.PlayerFitRefusalReason.Contains(TEXT("SampleStep=4"));
    TestTrue(TEXT("coarse player-fit control refuses unresolved capsule fit"), bCoarseRefused);
    AddInfo(FString::Printf(
        TEXT("CONTROL coarse_step4_player_fit: refused=%s reason=%s"),
        bCoarseRefused ? TEXT("true") : TEXT("false"),
        CoarseControlMetrics.PlayerFitRefusalReason.IsEmpty()
            ? TEXT("none") : *CoarseControlMetrics.PlayerFitRefusalReason));

    // A permissive stencil must fail these deliberately pathological supports. The wall has no
    // downward transition, the ledge covers only 1/5 of the footprint, and the overhang has
    // solid above rather than below the candidate. All controls use the same character-derived
    // settings and a bounded 16x16x12-voxel window.
    FVoxelStrateMeasureSettings RejectionSettings = CharacterFitSettings;
    RejectionSettings.SampleStep = 1;
    RejectionSettings.RadiusInVoxels = 8;
    RejectionSettings.CenterXY = FVector2D::ZeroVector;
    RejectionSettings.MaxCells = 100000;
    RejectionSettings.InteriorMarginVoxels = 0;
    auto RunStencilRejectionControl = [this, &RejectionSettings](
        const TCHAR* Label, const IVoxelStrateDensitySampler& Sampler)
    {
        const FVoxelStrateMetrics Metrics = VF_MeasureStrateWithSampler(
            Sampler, 0, 12, 0.0f, RejectionSettings, nullptr);
        const bool bRejected = Metrics.bValid && Metrics.bPlayerFitResolved
            && Metrics.NumPlayerFitCells == 0;
        TestTrue(FString::Printf(TEXT("stencil rejects %s"), Label), bRejected);
        AddInfo(FString::Printf(
            TEXT("CONTROL stencil_rejection %s: rejected=%s fit_cells=%lld "
                 "resolved=%s reason=%s"),
            Label,
            bRejected ? TEXT("true") : TEXT("false"),
            static_cast<long long>(Metrics.NumPlayerFitCells),
            Metrics.bPlayerFitResolved ? TEXT("true") : TEXT("false"),
            Metrics.PlayerFitRefusalReason.IsEmpty()
                ? TEXT("none") : *Metrics.PlayerFitRefusalReason));
    };
    FVerticalWallSampler VerticalWall;
    FIsolatedLedgeSampler IsolatedLedge;
    FOverhangSampler Overhang;
    FSteepRampSampler SteepRamp;
    RunStencilRejectionControl(TEXT("vertical_shaft_wall"), VerticalWall);
    RunStencilRejectionControl(TEXT("isolated_1_voxel_ledge"), IsolatedLedge);
    RunStencilRejectionControl(TEXT("overhang_underside"), Overhang);
    RunStencilRejectionControl(TEXT("over_44_degree_ramp"), SteepRamp);

    AddInfo(FString::Printf(
        TEXT("PLAYER_STENCIL radius=%.3f voxels (%.2f m), half_height=%.3f voxels "
             "(%.2f m), height=%.3f voxels (%.2f m); rows=ceil(height)=%d "
             "relative rows 0..%d; support=nearest scalar solid-to-air surface searched "
             "down <=%.3f m/%d cells; support patch=%d/%d columns with centre required; "
             "walkable slope<=%.1f deg; capsule clearance is re-tested at resolved support; "
             "HeadroomCells=%d is legacy-only"),
        ControlSettings.PlayerCapsuleRadiusVoxels,
        ControlSettings.PlayerCapsuleRadiusVoxels * 0.25f,
        ControlSettings.PlayerCapsuleHalfHeightVoxels,
        ControlSettings.PlayerCapsuleHalfHeightVoxels * 0.25f,
        FVoxelPlayerCapsuleConstants::HeightVoxels,
        FVoxelPlayerCapsuleConstants::HeightVoxels * 0.25f,
        FMath::CeilToInt(2.0f * ControlSettings.PlayerCapsuleHalfHeightVoxels),
        FMath::CeilToInt(2.0f * ControlSettings.PlayerCapsuleHalfHeightVoxels) - 1,
        ControlSettings.PlayerMaxStepHeightMeters,
        MaxDownwardSearchCells,
        RequiredSupportColumns,
        SupportFootprintColumns,
        ControlSettings.PlayerWalkableFloorAngleDegrees,
        ControlSettings.HeadroomCells));

    // PART A — direct route-window experiment on the requested Maze identity. The first row is
    // intentionally the old mouth-only box; the second turns on the topology-aware origin
    // coverage; the third uses the largest centered origin window that the same MaxCells cap can
    // hold while retaining enough resolution to contain both mouths.
    {
        const int32 PartAMazeSlot = FTestWorld::SlotMaze;
        VF_SetCandidateOnSlot(World, MazeCandidate, PartAMazeSlot);
        FVector ArrivalPoint = FVector::ZeroVector;
        FVector DeparturePoint = FVector::ZeroVector;
        int32 ArrivalCount = 0;
        int32 DepartureCount = 0;
        for (const FVoxelPassage& Passage : World.StrateManager->GetPassages())
        {
            if (Passage.LowerStrateIndex == PartAMazeSlot
                && Passage.UpperStrateIndex + 1 == PartAMazeSlot)
            {
                ArrivalPoint = Passage.LowerPoint;
                ++ArrivalCount;
            }
            if (Passage.UpperStrateIndex == PartAMazeSlot
                && Passage.LowerStrateIndex == PartAMazeSlot + 1)
            {
                DeparturePoint = Passage.UpperPoint;
                ++DepartureCount;
            }
        }

        TestEqual(TEXT("PART A Maze has one arrival mouth"), ArrivalCount, 1);
        TestEqual(TEXT("PART A Maze has one departure mouth"), DepartureCount, 1);
        int32 MazeTopVoxelZ = 0;
        int32 MazeBottomVoxelZ = 0;
        TestTrue(TEXT("PART A Maze has a vertical measurement range"),
                 World.GetSlotVoxelZRange(PartAMazeSlot, MazeTopVoxelZ, MazeBottomVoxelZ));
        if (ArrivalCount == 1 && DepartureCount == 1
            && World.GetSlotVoxelZRange(PartAMazeSlot, MazeTopVoxelZ, MazeBottomVoxelZ))
        {
            FVoxelStrateMeasureSettings PartABase = CharacterFitSettings;
            PartABase.SampleStep = 1;
            PartABase.RadiusInVoxels = 64;
            PartABase.MaxCells = FineDiagnosisMaxCells;
            PartABase.MaxRouteRetries = 16;
            PartABase.HeadroomCells = 2;
            PartABase.InteriorMarginVoxels = -1;

            FGeneratorSampler GeneratorSampler(*World.Generator);
            auto RunPartAWindow =
                [&](const FString& WindowLabel, const FVoxelStrateMeasureSettings& Settings)
            {
                FVoxelStrateMetrics Metrics;
                const FVoxelConnectivityDiagnostics Diagnostics =
                    VF_DiagnosePlayerFitConnectivityWithSampler(
                        GeneratorSampler, MazeBottomVoxelZ, MazeTopVoxelZ + 1,
                        VF_BoundarySealForCandidate(MazeCandidate),
                        ArrivalPoint, DeparturePoint, Settings, &Metrics);
                VF_ReportPlayerFitRow(
                    *this, TEXT("Maze seed=0 index=50 slot=4"), WindowLabel,
                    Settings.SampleStep, Metrics, Diagnostics);
                return TTuple<FVoxelStrateMetrics, FVoxelConnectivityDiagnostics>(
                    MoveTemp(Metrics), Diagnostics);
            };

            FVoxelStrateMeasureSettings CurrentSettings = PartABase;
            VoxelForgePlayerFitWindow::ConfigureMouthWindow(
                CurrentSettings, ArrivalPoint, DeparturePoint, ECaveGeneratorType::Maze);
            CurrentSettings.bIncludeOriginInCoverWindow = false;
            const auto CurrentResult = RunPartAWindow(
                TEXT("current mouth-AABB + margin"), CurrentSettings);

            FVoxelStrateMeasureSettings OriginSettings = PartABase;
            VoxelForgePlayerFitWindow::ConfigureMouthWindow(
                OriginSettings, ArrivalPoint, DeparturePoint, ECaveGeneratorType::Maze);
            const auto OriginResult = RunPartAWindow(
                TEXT("origin-inclusive mouth-AABB + margin"), OriginSettings);

            FVoxelStrateMeasureSettings WholeSettings = PartABase;
            WholeSettings.MaxCells = FineDiagnosisMaxCells * 8;
            WholeSettings.CenterXY = FVector2D::ZeroVector;
            WholeSettings.CoverPointA.Reset();
            WholeSettings.CoverPointB.Reset();
            WholeSettings.bIncludeOriginInCoverWindow = false;
            const float MouthMargin = FMath::CeilToFloat(
                WholeSettings.PlayerCapsuleRadiusVoxels) + 2.0f;
            const int32 MouthReach = FMath::CeilToInt(FMath::Max(
                FMath::Max(FMath::Abs(ArrivalPoint.X), FMath::Abs(DeparturePoint.X)),
                FMath::Max(FMath::Abs(ArrivalPoint.Y), FMath::Abs(DeparturePoint.Y)))
                + MouthMargin);
            WholeSettings.SampleStep = 1;
            WholeSettings.RadiusInVoxels = VF_MaxCenteredRadiusForCap(
                MazeTopVoxelZ, MazeBottomVoxelZ,
                VF_BoundarySealForCandidate(MazeCandidate), WholeSettings);
            const FString WholeWindowLabel = FString::Printf(
                TEXT("whole-strate centered origin cap window (radius=%d; mouth_reach=%d; "
                     "MaxCells=%d)"),
                WholeSettings.RadiusInVoxels, MouthReach, WholeSettings.MaxCells);
            const auto WholeResult = RunPartAWindow(WholeWindowLabel, WholeSettings);

            AddInfo(FString::Printf(
                TEXT("PART_A verdict: current=%s; origin-inclusive=%s; whole-strate-cap=%s; "
                     "origin flip=%s"),
                VF_DiagnosisConnectivityName(CurrentResult.Get<1>().Result),
                VF_DiagnosisConnectivityName(OriginResult.Get<1>().Result),
                VF_DiagnosisConnectivityName(WholeResult.Get<1>().Result),
                OriginResult.Get<1>().Result == EVoxelConnectivityResult::Connected
                    ? TEXT("CONFIRMED") : TEXT("NOT_CONFIRMED")));
        }
    }

    // H3 — the eight fixed showcase identities, measured at step 1 on the topology-aware fitted
    // window. Origin-rooted families include (0,0) before the MaxCells refusal check.
    // The Maze identity is intentionally the requested seed 0 / index 50 / fourth one-based slot.
    struct FShowcaseSelection
    {
        ECaveGeneratorType Archetype;
        int32 Seed;
        int32 CandidateIndex;
        int32 TargetSlot;
        const TCHAR* Label;
    };
    const FShowcaseSelection Selections[] = {
        { ECaveGeneratorType::CrystalChamber, 0,   28, FTestWorld::SlotFlatPlain, TEXT("CrystalChamber") },
        { ECaveGeneratorType::FlatPlain,       0,   37, FTestWorld::SlotFlatPlain, TEXT("FlatPlain") },
        { ECaveGeneratorType::FloatingIslands, 0,    9, FTestWorld::SlotFlatPlain, TEXT("FloatingIslands") },
        { ECaveGeneratorType::Maze,            0,   50, FTestWorld::SlotMaze,      TEXT("Maze") },
        { ECaveGeneratorType::SurfaceWorld,    0,   27, FTestWorld::SlotSurfaceWorld, TEXT("SurfaceWorld") },
        { ECaveGeneratorType::TunnelNetwork,   0,    4, FTestWorld::SlotFlatPlain, TEXT("TunnelNetwork") },
        { ECaveGeneratorType::Underwater,      0,   16, FTestWorld::SlotFlatPlain, TEXT("Underwater") },
        { ECaveGeneratorType::VerticalShafts,  0,    6, FTestWorld::SlotFlatPlain, TEXT("VerticalShafts") },
    };

    // Baseline rows from the pre-fix fitted-window report. The supplied VerticalShafts row is
    // retained verbatim (it came from the owner's scale report); it is deliberately labelled as
    // a comparison row rather than recomputed by this test.
    struct FShowcaseBaseline
    {
        int64 FitCells;
        int32 Components;
        int64 LargestComponent;
        int64 ArrivalComponent;
        int64 DepartureComponent;
        float MouthGap;
        const TCHAR* Law;
    };
    const FShowcaseBaseline Baselines[] = {
        {655, 197, 24, 1, 1, 47.529f, TEXT("NotConnectedAtThisResolution")},
        {1116, 45, 259, 1, 1, 48.104f, TEXT("NotConnectedAtThisResolution")},
        {4, 2, 3, 1, 1, 0.000f, TEXT("Connected")},
        {2801, 309, 176, 43, 81, 206.630f, TEXT("NotConnectedAtThisResolution")},
        {713, 520, 6, 1, 1, 110.648f, TEXT("NotConnectedAtThisResolution")},
        {2748, 467, 1292, 2, 1, 164.405f, TEXT("NotConnectedAtThisResolution")},
        {337, 86, 35, 3, 11, 142.499f, TEXT("NotConnectedAtThisResolution")},
        {1156, 112, 96, 3, 36, 94.000f, TEXT("NotConnectedAtThisResolution")},
    };
    static_assert(UE_ARRAY_COUNT(Selections) == UE_ARRAY_COUNT(Baselines),
                  "H3 before/after rows must remain paired.");
    int32 NumH3Connected = 0;
    int32 NumH3DegenerateConnected = 0;
    int32 NumH3Measured = 0;
    int32 NumH3Refused = 0;

    for (const FShowcaseSelection& Selection : Selections)
    {
        const FVoxelStrateComposerCandidate Candidate = VF_RollStrateCandidate(
            Corpus, Selection.Seed, Selection.CandidateIndex, false);
        TestTrue(FString::Printf(TEXT("H3 %s candidate identity is valid"), Selection.Label),
                 Candidate.bValid && Candidate.Archetype == Selection.Archetype
                     && Candidate.Regions.IsSingleRegion());
        if (!Candidate.bValid || Candidate.Archetype != Selection.Archetype
            || !Candidate.Regions.IsSingleRegion())
        {
            continue;
        }
        VF_SetCandidateOnSlot(World, Candidate, Selection.TargetSlot);
        FVector ArrivalPoint = FVector::ZeroVector;
        FVector DeparturePoint = FVector::ZeroVector;
        int32 ArrivalCount = 0;
        int32 DepartureCount = 0;
        for (const FVoxelPassage& Passage : World.StrateManager->GetPassages())
        {
            if (Passage.LowerStrateIndex == Selection.TargetSlot
                && Passage.UpperStrateIndex + 1 == Selection.TargetSlot)
            {
                ArrivalPoint = Passage.LowerPoint;
                ++ArrivalCount;
            }
            if (Passage.UpperStrateIndex == Selection.TargetSlot
                && Passage.LowerStrateIndex == Selection.TargetSlot + 1)
            {
                DeparturePoint = Passage.UpperPoint;
                ++DepartureCount;
            }
        }
        if (ArrivalCount != 1 || DepartureCount != 1)
        {
            AddInfo(FString::Printf(
                TEXT("PLAYER_FIT step=1 fitted-window %s: mouths unavailable arrival=%d departure=%d"),
                Selection.Label, ArrivalCount, DepartureCount));
            continue;
        }

        FVoxelStrateMeasureSettings Settings = CharacterFitSettings;
        Settings.SampleStep = 1;
        Settings.RadiusInVoxels = 64;
        VoxelForgePlayerFitWindow::ConfigureMouthWindow(
            Settings, ArrivalPoint, DeparturePoint, Selection.Archetype);
        Settings.MaxCells = FineDiagnosisMaxCells;
        Settings.MaxRouteRetries = 16;
        Settings.HeadroomCells = 2;
        Settings.InteriorMarginVoxels = -1;

        int32 TopVoxelZ = 0;
        int32 BottomVoxelZ = 0;
        if (!World.GetSlotVoxelZRange(Selection.TargetSlot, TopVoxelZ, BottomVoxelZ))
        {
            AddError(FString::Printf(TEXT("PLAYER_FIT %s: target slot has no Z range."),
                                     Selection.Label));
            continue;
        }
        FGeneratorSampler GeneratorSampler(*World.Generator);
        FVoxelStrateMetrics Metrics;
        const FVoxelConnectivityDiagnostics Diagnostics =
            VF_DiagnosePlayerFitConnectivityWithSampler(
                GeneratorSampler, BottomVoxelZ, TopVoxelZ + 1,
                VF_BoundarySealForCandidate(Candidate), ArrivalPoint, DeparturePoint,
                Settings, &Metrics);
        VF_ReportPlayerFitRow(
            *this, Selection.Label,
            VoxelForgePlayerFitWindow::RouteWindowName(Selection.Archetype),
            Settings.SampleStep, Metrics, Diagnostics);
        if (!Metrics.bValid || !Metrics.bPlayerFitResolved)
        {
            ++NumH3Refused;
            const FString& Refusal = !Metrics.RefusalReason.IsEmpty()
                ? Metrics.RefusalReason : Metrics.PlayerFitRefusalReason;
            AddInfo(FString::Printf(
                TEXT("H3 %s law=REFUSED; window=%s; reason=%s"),
                Selection.Label,
                VoxelForgePlayerFitWindow::RouteWindowName(Selection.Archetype),
                Refusal.IsEmpty() ? TEXT("player-fit metrics unresolved") : *Refusal));
            continue;
        }
        ++NumH3Measured;
        if (Diagnostics.Result == EVoxelConnectivityResult::Connected)
        {
            ++NumH3Connected;
            if (Metrics.NumPlayerFitCells <= 4)
            {
                ++NumH3DegenerateConnected;
            }
        }
        const FShowcaseBaseline& Before = Baselines[&Selection - Selections];
        FString DeltaReport = TEXT("H3_DELTA step1 ");
        DeltaReport += Selection.Label;
        DeltaReport += FString::Printf(
            TEXT(": before=%lld/%d/%lld/%lld/%lld/%.3f/"),
            static_cast<long long>(Before.FitCells), Before.Components,
            static_cast<long long>(Before.LargestComponent),
            static_cast<long long>(Before.ArrivalComponent),
            static_cast<long long>(Before.DepartureComponent), Before.MouthGap);
        DeltaReport += Before.Law;
        DeltaReport += FString::Printf(
            TEXT("; after=%lld/%d/%lld/%lld/%lld/%.3f/"),
            static_cast<long long>(Metrics.NumPlayerFitCells), Metrics.NumTraversableComponents,
            static_cast<long long>(Metrics.LargestTraversableComponentCells),
            static_cast<long long>(Diagnostics.StartComponentCells),
            static_cast<long long>(Diagnostics.GoalComponentCells),
            Diagnostics.StartToGoalComponentDistanceCells);
        DeltaReport += VF_DiagnosisConnectivityName(Diagnostics.Result);
        DeltaReport += FString::Printf(
            TEXT("; delta_fit=%lld delta_components=%d delta_largest=%lld "
                 "delta_arrival=%lld delta_departure=%lld delta_gap=%.3f"),
            static_cast<long long>(Metrics.NumPlayerFitCells - Before.FitCells),
            Metrics.NumTraversableComponents - Before.Components,
            static_cast<long long>(Metrics.LargestTraversableComponentCells - Before.LargestComponent),
            static_cast<long long>(Diagnostics.StartComponentCells - Before.ArrivalComponent),
            static_cast<long long>(Diagnostics.GoalComponentCells - Before.DepartureComponent),
            Diagnostics.StartToGoalComponentDistanceCells - Before.MouthGap);
        AddInfo(DeltaReport);
    }

    // The old tally's one Connected row was the four-cell FloatingIslands pocket. Keep that
    // degenerate fact visible beside the new tally so a tiny connected island is not mistaken for
    // an instrument-wide success.
    AddInfo(FString::Printf(
        TEXT("LAW_TALLY restricted step1: before_connected=1/8 "
             "before_connected_through_4_cells=1; after_connected=%d/%d "
             "after_connected_through_4_cells=%d measured=%d/8 refused=%d/8"),
        NumH3Connected,
        NumH3Measured,
        NumH3DegenerateConnected,
        NumH3Measured,
        NumH3Refused));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
