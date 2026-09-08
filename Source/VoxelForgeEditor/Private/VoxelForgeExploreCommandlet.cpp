#include "VoxelForgeExploreCommandlet.h"

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/CommandLine.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "VoxelDiffLayer.h"
#include "VoxelGenerator.h"
#include "VoxelMarchingCubesMesher.h"
#include "VoxelNoise.h"
#include "VoxelSettings.h"
#include "VoxelStrateDefinition.h"
#include "VoxelStrateManager.h"
#include "VoxelStrateMeasure.h"
#include "VoxelTypes.h"

namespace
{
using FExploreJsonWriter = TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;

constexpr int32 DefaultRenderWidth = 512;
constexpr int32 DefaultRenderHeight = 288;
constexpr float DefaultRenderStepVoxels = 0.25f;
constexpr float DefaultRenderMaxDistanceVoxels = 160.0f;
constexpr int32 DefaultExportSize = 128;
constexpr int32 DefaultMaxWalkCells = 32000000;
constexpr int32 MaxSyntheticStrates = 64;
constexpr int32 MaxRenderPixels = 1048576;
constexpr int64 MaxRenderDensitySamples = 400000000ll;
constexpr int32 MaxExportSize = 128;
constexpr int64 MaxWalkWorkingBytes = 768ll * 1024ll * 1024ll;
// The exact fine-grid walk retains Air + Density + player-fit bits + non-finite flags, and can
// transiently hold Parent + Queue + Path or Visited + NarrowCell + DFS stacks. 21 bytes/cell is a
// conservative preflight figure for those arrays, including allocator slack/headroom.
constexpr int64 EstimatedWalkBytesPerCell = 21ll;
constexpr int64 MaxExportWorkingBytes = 1024ll * 1024ll * 1024ll;

const TCHAR* ArchetypeName(ECaveGeneratorType Archetype)
{
    switch (Archetype)
    {
    case ECaveGeneratorType::TunnelNetwork:   return TEXT("TunnelNetwork");
    case ECaveGeneratorType::FlatPlain:       return TEXT("FlatPlain");
    case ECaveGeneratorType::CrystalChamber:  return TEXT("CrystalChamber");
    case ECaveGeneratorType::Maze:            return TEXT("Maze");
    case ECaveGeneratorType::SurfaceWorld:    return TEXT("SurfaceWorld");
    case ECaveGeneratorType::VerticalShafts:  return TEXT("VerticalShafts");
    case ECaveGeneratorType::FloatingIslands: return TEXT("FloatingIslands");
    case ECaveGeneratorType::Underwater:      return TEXT("Underwater");
    default:                                  return TEXT("Unknown");
    }
}

const TCHAR* ConnectivityResultName(EVoxelConnectivityResult Result)
{
    switch (Result)
    {
    case EVoxelConnectivityResult::Connected:                  return TEXT("Connected");
    case EVoxelConnectivityResult::NotConnectedAtThisResolution:
        return TEXT("NotConnectedAtThisResolution");
    case EVoxelConnectivityResult::StartCellSolid:             return TEXT("StartCellSolid");
    case EVoxelConnectivityResult::GoalCellSolid:              return TEXT("GoalCellSolid");
    case EVoxelConnectivityResult::OutOfWindow:                return TEXT("OutOfWindow");
    case EVoxelConnectivityResult::CoarseLiedBudgetExhausted:  return TEXT("CoarseLiedBudgetExhausted");
    case EVoxelConnectivityResult::StartCellNotPlayerFit:      return TEXT("StartCellNotPlayerFit");
    case EVoxelConnectivityResult::GoalCellNotPlayerFit:       return TEXT("GoalCellNotPlayerFit");
    default:                                                   return TEXT("Unknown");
    }
}

bool ParseArchetype(const FString& Text, ECaveGeneratorType& OutArchetype)
{
    FString Normalized = Text;
    Normalized.TrimStartAndEndInline();
    Normalized.ToLowerInline();
    Normalized.ReplaceInline(TEXT("_"), TEXT(""));
    Normalized.ReplaceInline(TEXT("-"), TEXT(""));

    const TPair<const TCHAR*, ECaveGeneratorType> Names[] = {
        { TEXT("tunnelnetwork"),   ECaveGeneratorType::TunnelNetwork },
        { TEXT("flatplain"),        ECaveGeneratorType::FlatPlain },
        { TEXT("crystalchamber"),   ECaveGeneratorType::CrystalChamber },
        { TEXT("maze"),             ECaveGeneratorType::Maze },
        { TEXT("surfaceworld"),     ECaveGeneratorType::SurfaceWorld },
        { TEXT("verticalshafts"),   ECaveGeneratorType::VerticalShafts },
        { TEXT("floatingislands"),  ECaveGeneratorType::FloatingIslands },
        { TEXT("floatingisland"),   ECaveGeneratorType::FloatingIslands },
        { TEXT("underwater"),       ECaveGeneratorType::Underwater },
    };
    for (const auto& Pair : Names)
    {
        if (Normalized == Pair.Key)
        {
            OutArchetype = Pair.Value;
            return true;
        }
    }
    return false;
}

bool NeedsOriginInWindow(ECaveGeneratorType Archetype)
{
    // This is the same topology policy as VoxelForgePlayerFitWindow.h. That header is test-only,
    // so the editor commandlet keeps the policy here while still delegating the actual fit test to
    // VoxelStrateMeasure's shared VF_ stencil.
    switch (Archetype)
    {
    case ECaveGeneratorType::Maze:
    case ECaveGeneratorType::VerticalShafts:
    case ECaveGeneratorType::TunnelNetwork:
    case ECaveGeneratorType::Underwater:
        return true;
    case ECaveGeneratorType::FlatPlain:
    case ECaveGeneratorType::CrystalChamber:
    case ECaveGeneratorType::SurfaceWorld:
    case ECaveGeneratorType::FloatingIslands:
    default:
        return false;
    }
}

float BoundarySealForArchetype(
    const UVoxelStrateDefinition& Definition,
    ECaveGeneratorType Archetype)
{
    switch (Archetype)
    {
    case ECaveGeneratorType::FlatPlain:
    case ECaveGeneratorType::CrystalChamber:
        return Definition.SlabParams.BoundarySealThickness;
    case ECaveGeneratorType::Maze:
        return Definition.MazeParams.BoundarySealThickness;
    case ECaveGeneratorType::SurfaceWorld:
        return Definition.SurfaceParams.BoundarySealThickness;
    case ECaveGeneratorType::VerticalShafts:
        return Definition.VerticalShaftParams.BoundarySealThickness;
    case ECaveGeneratorType::FloatingIslands:
        return Definition.FloatingIslandParams.BoundarySealThickness;
    case ECaveGeneratorType::TunnelNetwork:
    case ECaveGeneratorType::Underwater:
    default:
        return Definition.GenerationParams.BoundarySealThickness;
    }
}

struct FExploreArguments
{
    int32 Seed = 0;
    ECaveGeneratorType Archetype = ECaveGeneratorType::Maze;
    int32 Slot = 4;
    bool bRender = true;
    bool bWalk = true;
    bool bExport = true;
    bool bFailureFocusRender = false;
    FString OutDirectory;

    int32 RenderWidth = DefaultRenderWidth;
    int32 RenderHeight = DefaultRenderHeight;
    float RenderStepVoxels = DefaultRenderStepVoxels;
    float RenderMaxDistanceVoxels = DefaultRenderMaxDistanceVoxels;
    int32 ExportSize = DefaultExportSize;
    int32 MaxWalkCells = DefaultMaxWalkCells;

    FString CanonicalModes() const
    {
        FString Result;
        if (bRender) Result += TEXT("render,");
        if (bWalk) Result += TEXT("walk,");
        if (bExport) Result += TEXT("export,");
        if (Result.EndsWith(TEXT(",")))
        {
            Result.LeftChopInline(1);
        }
        return Result;
    }
};

bool ParseArguments(const FString& Params, FExploreArguments& OutArguments, FString& OutError)
{
    FString ArchetypeText = TEXT("Maze");
    FString ModesText = TEXT("render,walk,export");
    FString OutText;

    FParse::Value(*Params, TEXT("seed="), OutArguments.Seed);
    FParse::Value(*Params, TEXT("archetype="), ArchetypeText);
    FParse::Value(*Params, TEXT("slot="), OutArguments.Slot);
    FParse::Value(*Params, TEXT("modes="), ModesText);
    FParse::Value(*Params, TEXT("out="), OutText);
    FParse::Value(*Params, TEXT("renderwidth="), OutArguments.RenderWidth);
    FParse::Value(*Params, TEXT("renderheight="), OutArguments.RenderHeight);
    FParse::Value(*Params, TEXT("renderstep="), OutArguments.RenderStepVoxels);
    FParse::Value(*Params, TEXT("rendermaxdistance="), OutArguments.RenderMaxDistanceVoxels);
    FParse::Value(*Params, TEXT("exportsize="), OutArguments.ExportSize);
    FParse::Value(*Params, TEXT("maxwalkcells="), OutArguments.MaxWalkCells);
    int32 FailureFocusRender = 0;
    FParse::Value(*Params, TEXT("failurefocus="), FailureFocusRender);
    OutArguments.bFailureFocusRender = FailureFocusRender != 0;

    if (!ParseArchetype(ArchetypeText, OutArguments.Archetype))
    {
        OutError = FString::Printf(
            TEXT("Unknown archetype '%s'. Expected Maze, TunnelNetwork, FlatPlain, "
                 "CrystalChamber, SurfaceWorld, VerticalShafts, FloatingIslands, or Underwater."),
            *ArchetypeText);
        return false;
    }

    OutArguments.bRender = false;
    OutArguments.bWalk = false;
    OutArguments.bExport = false;
    TArray<FString> Modes;
    ModesText.ParseIntoArray(Modes, TEXT(","), true);
    for (FString Mode : Modes)
    {
        Mode.TrimStartAndEndInline();
        Mode.ToLowerInline();
        if (Mode == TEXT("all"))
        {
            OutArguments.bRender = true;
            OutArguments.bWalk = true;
            OutArguments.bExport = true;
        }
        else if (Mode == TEXT("render"))
        {
            OutArguments.bRender = true;
        }
        else if (Mode == TEXT("walk"))
        {
            OutArguments.bWalk = true;
        }
        else if (Mode == TEXT("export"))
        {
            OutArguments.bExport = true;
        }
        else
        {
            OutError = FString::Printf(
                TEXT("Unknown mode '%s'. Expected render, walk, export, or all."), *Mode);
            return false;
        }
    }
    if (!OutArguments.bRender && !OutArguments.bWalk && !OutArguments.bExport)
    {
        OutError = TEXT("At least one mode must be selected.");
        return false;
    }

    if (OutArguments.Slot < 1 || OutArguments.Slot > MaxSyntheticStrates - 2)
    {
        OutError = FString::Printf(
            TEXT("slot must be an interior slot in [1,%d] so arrival and departure mouths exist."),
            MaxSyntheticStrates - 2);
        return false;
    }
    if (OutArguments.RenderWidth < 16 || OutArguments.RenderHeight < 16
        || static_cast<int64>(OutArguments.RenderWidth)
            * static_cast<int64>(OutArguments.RenderHeight) > MaxRenderPixels)
    {
        OutError = FString::Printf(
            TEXT("renderwidth*renderheight must be between 256 pixels and %d pixels."),
            MaxRenderPixels);
        return false;
    }
    if (!FMath::IsFinite(OutArguments.RenderStepVoxels)
        || OutArguments.RenderStepVoxels < 0.125f
        || OutArguments.RenderStepVoxels > 8.0f)
    {
        OutError = TEXT("renderstep must be finite and in [0.125,8] voxels.");
        return false;
    }
    if (!FMath::IsFinite(OutArguments.RenderMaxDistanceVoxels)
        || OutArguments.RenderMaxDistanceVoxels <= 0.0f
        || OutArguments.RenderMaxDistanceVoxels > 2048.0f)
    {
        OutError = TEXT("rendermaxdistance must be finite and in (0,2048] voxels.");
        return false;
    }
    const int64 StepsPerRay = static_cast<int64>(FMath::Max(
        1,
        FMath::CeilToInt(
            OutArguments.RenderMaxDistanceVoxels / OutArguments.RenderStepVoxels)));
    const int64 EstimatedRenderSamples = static_cast<int64>(OutArguments.RenderWidth)
        * static_cast<int64>(OutArguments.RenderHeight)
        * 3ll
        * StepsPerRay;
    if (EstimatedRenderSamples > MaxRenderDensitySamples)
    {
        OutError = FString::Printf(
            TEXT("render request would take about %lld fixed density samples, over the cap %lld."),
            static_cast<long long>(EstimatedRenderSamples),
            static_cast<long long>(MaxRenderDensitySamples));
        return false;
    }
    if (OutArguments.ExportSize < CHUNK_SIZE
        || OutArguments.ExportSize > MaxExportSize
        || OutArguments.ExportSize % CHUNK_SIZE != 0)
    {
        OutError = FString::Printf(
            TEXT("exportsize must be a multiple of CHUNK_SIZE (%d) in [%d,%d] cells per axis."),
            CHUNK_SIZE,
            CHUNK_SIZE,
            MaxExportSize);
        return false;
    }
    if (OutArguments.MaxWalkCells <= 0)
    {
        OutError = TEXT("maxwalkcells must be greater than zero.");
        return false;
    }

    if (OutText.IsEmpty())
    {
        OutText = FString::Printf(
            TEXT("Saved/VoxelForge/Explore/seed%d_%s_slot%d"),
            OutArguments.Seed,
            ArchetypeName(OutArguments.Archetype),
            OutArguments.Slot);
    }
    if (FPaths::IsRelative(OutText))
    {
        OutText = FPaths::Combine(FPaths::ProjectDir(), OutText);
    }
    OutArguments.OutDirectory = FPaths::ConvertRelativePathToFull(OutText);
    FPaths::NormalizeDirectoryName(OutArguments.OutDirectory);
    return true;
}

struct FExploreWorld
{
    TStrongObjectPtr<UVoxelSettings> Settings;
    TStrongObjectPtr<UVoxelStrateManager> Manager;
    TStrongObjectPtr<UVoxelDiffLayer> DiffLayer;
    TStrongObjectPtr<UVoxelGenerator> Generator;
    TStrongObjectPtr<UVoxelMarchingCubesMesher> Mesher;
    TArray<TStrongObjectPtr<UVoxelStrateDefinition>> Definitions;

    int32 TargetBottomWorldZ = 0;
    int32 TargetTopWorldZ = 0;
    float TargetBoundarySealThickness = 4.0f;

    bool Build(const FExploreArguments& Arguments, FString& OutError)
    {
        const int32 NumStrates = Arguments.Slot + 2;
        if (NumStrates > MaxSyntheticStrates)
        {
            OutError = TEXT("The synthetic layout would exceed the commandlet strate cap.");
            return false;
        }

        Settings = TStrongObjectPtr<UVoxelSettings>(
            NewObject<UVoxelSettings>(GetTransientPackage(), NAME_None, RF_Transient));
        Settings->Seed = Arguments.Seed;
        Settings->TotalStrates = NumStrates;
        Settings->InterStrateGapChunks = 0;
        Settings->WorldRadiusVoxels = 0.0f;
        Settings->EdgeSealThickness = 64.0f;
        Settings->OriginSpineRadius = 14.0f;
        Settings->bOpenSurfaceEntry = true;

        Definitions.Reserve(NumStrates);
        for (int32 Index = 0; Index < NumStrates; ++Index)
        {
            UVoxelStrateDefinition* Definition = NewObject<UVoxelStrateDefinition>(
                GetTransientPackage(), NAME_None, RF_Transient);
            Definition->StrateName = FText::FromString(FString::Printf(
                TEXT("VoxelForgeExplore_%s_slot%d"),
                ArchetypeName(Index == Arguments.Slot
                    ? Arguments.Archetype
                    : ECaveGeneratorType::TunnelNetwork),
                Index));
            Definition->StrateDescription = FText::FromString(
                TEXT("Transient, deterministic commandlet definition."));
            // The commandlet's acceptance sweep uses the owner-facing base spacing:
            // 4 chunks * 32 voxels * 0.25 m = 32 m per strate. Production assets may be taller;
            // this keeps the measured tunnel arithmetic tied to the stated ~32 m descent.
            Definition->StrateHeightInChunks = 4;
            Definition->TransitionType = EVoxelStrateTransition::Hard;
            Definition->GeneratorType = Index == Arguments.Slot
                ? Arguments.Archetype
                : ECaveGeneratorType::TunnelNetwork;
            // Exercise the production operator-stack opt-in where it is implemented (Maze),
            // while the runtime switch safely falls back for other archetypes.
            Definition->bUseOperatorStack = true;

            const TSoftObjectPtr<UVoxelStrateDefinition> SoftDefinition(Definition);
            Settings->FixedStrates.Add(Index, SoftDefinition);
            Settings->StratePool.Add(SoftDefinition);
            Definitions.Add(TStrongObjectPtr<UVoxelStrateDefinition>(Definition));
        }

        Manager = TStrongObjectPtr<UVoxelStrateManager>(
            NewObject<UVoxelStrateManager>(GetTransientPackage(), NAME_None, RF_Transient));
        if (!Manager->Initialize(Settings.Get(), Settings->Seed))
        {
            OutError = TEXT("UVoxelStrateManager::Initialize refused the transient layout.");
            return false;
        }

        if (!Manager->GetLayout().IsValidIndex(Arguments.Slot))
        {
            OutError = TEXT("The requested target slot did not enter the manager layout.");
            return false;
        }
        const FStrateSlot& Target = Manager->GetLayout()[Arguments.Slot];
        if (Target.Definition == nullptr)
        {
            OutError = TEXT("The requested target slot has no resolved definition.");
            return false;
        }
        TargetBottomWorldZ = Target.BottomChunkZ * CHUNK_SIZE;
        TargetTopWorldZ = (Target.TopChunkZ + 1) * CHUNK_SIZE;
        TargetBoundarySealThickness = BoundarySealForArchetype(
            *Target.Definition, Arguments.Archetype);
        if (TargetTopWorldZ <= TargetBottomWorldZ)
        {
            OutError = TEXT("The target strate has no positive voxel-space height.");
            return false;
        }

        DiffLayer = TStrongObjectPtr<UVoxelDiffLayer>(
            NewObject<UVoxelDiffLayer>(GetTransientPackage(), NAME_None, RF_Transient));
        Generator = TStrongObjectPtr<UVoxelGenerator>(
            NewObject<UVoxelGenerator>(GetTransientPackage(), NAME_None, RF_Transient));
        Generator->InitializeSettings(Settings.Get());
        Generator->SetStrateManager(Manager.Get());
        Generator->SetDiffLayer(DiffLayer.Get());
        Mesher = TStrongObjectPtr<UVoxelMarchingCubesMesher>(
            NewObject<UVoxelMarchingCubesMesher>(GetTransientPackage(), NAME_None, RF_Transient));
        Mesher->SetGenerator(Generator.Get());

        if (Settings->WorldRadiusVoxels != 0.0f
            || Generator->WorldRadiusVoxels != 0.0f
            || Settings->InterStrateGapChunks != 0)
        {
            OutError = TEXT(
                "Explorer invariant failed: WorldRadiusVoxels must stay 0 and lateral regions must stay gated off.");
            return false;
        }
        return true;
    }
};

class FGeneratorDensitySampler final : public IVoxelStrateDensitySampler
{
public:
    explicit FGeneratorDensitySampler(const UVoxelGenerator& InGenerator)
        : Generator(InGenerator)
    {
    }

    virtual float SampleDensity(float WorldX, float WorldY, float WorldZ) const override
    {
        return Generator.GetDensityAt(WorldX, WorldY, WorldZ);
    }

private:
    const UVoxelGenerator& Generator;
};

struct FExploreRenderFrame
{
    FString FileName;
    FString Purpose = TEXT("overview");
    FString CameraSeedSource = TEXT("player_fit");
    FVector CameraSeedPoseVoxels = FVector::ZeroVector;
    FVector CameraVoxels = FVector::ZeroVector;
    FVector TargetVoxels = FVector::ZeroVector;
    int32 HitPixels = 0;
    bool bScaleMarkerProjected = false;
    bool bFailureMarkersProjected = false;
};

struct FExploreRenderOutput
{
    FString Status;
    FString RefusalReason;
    int32 Width = 0;
    int32 Height = 0;
    float FixedStepVoxels = 0.0f;
    float FixedStepMeters = 0.0f;
    int32 BisectionIterations = 0;
    float MaxDistanceVoxels = 0.0f;
    int64 EstimatedFixedDensitySamples = 0;
    FString StepRationale;
    FString CameraSeedPolicy;
    int32 CameraSeedCount = 0;
    bool bAllCamerasPlayerFit = false;
    TArray<FExploreRenderFrame> Frames;
};

struct FExploreWalkOutput
{
    FString Status;
    FString RefusalReason;
    bool bHasArrival = false;
    bool bHasDeparture = false;
    int32 ArrivalPassageCount = 0;
    int32 DeparturePassageCount = 0;
    FVector ArrivalVoxels = FVector::ZeroVector;
    FVector DepartureVoxels = FVector::ZeroVector;
    FString WindowPolicy;
    FVoxelPlayerFitWalkReport Report;
    bool bOriginCheckAttempted = false;
    bool bOriginCheckAvailable = false;
    FString OriginCheckRefusalReason;
    FVoxelPlayerFitWalkReport OriginCheckReport;
    int32 ArrivalLandingRoomProbeCount = 0;
    int32 ArrivalLandingRoomPlayerFitProbeCount = 0;
    int32 ArrivalLandingRoomArrivalComponentProbeCount = 0;
    int32 DepartureLandingRoomProbeCount = 0;
    int32 DepartureLandingRoomPlayerFitProbeCount = 0;
    int32 DepartureLandingRoomArrivalComponentProbeCount = 0;
};

struct FExploreExportOutput
{
    FString Status;
    FString RefusalReason;
    FString MeshFileName;
    FString ManifestFileName;
    FIntVector RegionOrigin = FIntVector::ZeroValue;
    int32 RegionSize = 0;
    int32 MesherTileCount = 0;
    int64 EstimatedWorkingBytes = 0;
    int64 WorkingMemoryCapBytes = MaxExportWorkingBytes;
    int64 MeshArrayBytes = 0;
    int64 MeshFileSizeBytes = 0;
    int32 VertexCount = 0;
    int32 TriangleCount = 0;
};

struct FExploreRunOutput
{
    FExploreRenderOutput Render;
    FExploreWalkOutput Walk;
    FExploreExportOutput Export;
};

bool FindMouths(
    const UVoxelStrateManager& Manager,
    int32 TargetSlot,
    FVector& OutArrival,
    FVector& OutDeparture,
    int32& OutArrivalCount,
    int32& OutDepartureCount,
    const FVoxelPassage*& OutArrivalPassage,
    const FVoxelPassage*& OutDeparturePassage)
{
    OutArrival = FVector::ZeroVector;
    OutDeparture = FVector::ZeroVector;
    OutArrivalCount = 0;
    OutDepartureCount = 0;
    OutArrivalPassage = nullptr;
    OutDeparturePassage = nullptr;
    for (const FVoxelPassage& Passage : Manager.GetPassages())
    {
        if (Passage.LowerStrateIndex == TargetSlot
            && Passage.UpperStrateIndex + 1 == TargetSlot)
        {
            if (OutArrivalCount == 0)
            {
                OutArrival = Passage.LowerPoint;
                OutArrivalPassage = &Passage;
            }
            ++OutArrivalCount;
        }
        if (Passage.UpperStrateIndex == TargetSlot
            && Passage.LowerStrateIndex == TargetSlot + 1)
        {
            if (OutDepartureCount == 0)
            {
                OutDeparture = Passage.UpperPoint;
                OutDeparturePassage = &Passage;
            }
            ++OutDepartureCount;
        }
    }
    return OutArrivalCount > 0 && OutDepartureCount > 0;
}

struct FExploreCameraBasis
{
    FVector Forward = FVector::ForwardVector;
    FVector Right = FVector::RightVector;
    FVector Up = FVector::UpVector;
    float Aspect = 1.0f;
    float TanHalfFov = 1.0f;
};

bool BuildCameraBasis(
    const FVector& Position,
    const FVector& Target,
    float FieldOfViewDegrees,
    int32 Width,
    int32 Height,
    FExploreCameraBasis& OutBasis)
{
    OutBasis.Forward = (Target - Position).GetSafeNormal();
    if (OutBasis.Forward.IsNearlyZero())
    {
        return false;
    }
    OutBasis.Right = FVector::CrossProduct(OutBasis.Forward, FVector::UpVector).GetSafeNormal();
    if (OutBasis.Right.IsNearlyZero())
    {
        OutBasis.Right = FVector::CrossProduct(OutBasis.Forward, FVector::RightVector).GetSafeNormal();
    }
    OutBasis.Up = FVector::CrossProduct(OutBasis.Right, OutBasis.Forward).GetSafeNormal();
    OutBasis.Aspect = static_cast<float>(Width) / static_cast<float>(Height);
    OutBasis.TanHalfFov = FMath::Tan(FMath::DegreesToRadians(FieldOfViewDegrees * 0.5f));
    return OutBasis.Aspect > 0.0f && OutBasis.TanHalfFov > 0.0f;
}

FVector MakeRayDirection(
    const FExploreCameraBasis& Basis,
    int32 PixelX,
    int32 PixelY,
    int32 Width,
    int32 Height)
{
    const float NormalizedX = (2.0f * (static_cast<float>(PixelX) + 0.5f)
        / static_cast<float>(Width)) - 1.0f;
    const float NormalizedY = 1.0f - (2.0f * (static_cast<float>(PixelY) + 0.5f)
        / static_cast<float>(Height));
    return (Basis.Forward
        + Basis.Right * (NormalizedX * Basis.Aspect * Basis.TanHalfFov)
        + Basis.Up * (NormalizedY * Basis.TanHalfFov)).GetSafeNormal();
}

bool TraceDensityRay(
    const UVoxelGenerator& Generator,
    const FVector& Origin,
    const FVector& Direction,
    float FixedStepVoxels,
    float MaxDistanceVoxels,
    int32 BisectionIterations,
    FVector& OutHitPoint,
    FVector& OutNormal)
{
    float PreviousDensity = Generator.GetDensityAt(Origin.X, Origin.Y, Origin.Z);
    if (!FMath::IsFinite(PreviousDensity))
    {
        return false;
    }
    if (PreviousDensity == 0.0f)
    {
        OutHitPoint = Origin;
        OutNormal = FVector::UpVector;
        return true;
    }

    const int32 NumSteps = FMath::Max(
        1,
        FMath::CeilToInt(MaxDistanceVoxels / FixedStepVoxels));
    float PreviousDistance = 0.0f;
    for (int32 StepIndex = 1; StepIndex <= NumSteps; ++StepIndex)
    {
        const float CurrentDistance = FMath::Min(
            MaxDistanceVoxels,
            static_cast<float>(StepIndex) * FixedStepVoxels);
        const FVector CurrentPoint = Origin + Direction * CurrentDistance;
        const float CurrentDensity = Generator.GetDensityAt(
            CurrentPoint.X, CurrentPoint.Y, CurrentPoint.Z);
        if (!FMath::IsFinite(CurrentDensity))
        {
            return false;
        }

        const bool bPreviousAir = PreviousDensity > 0.0f;
        const bool bCurrentAir = CurrentDensity > 0.0f;
        if (bPreviousAir != bCurrentAir || CurrentDensity == 0.0f)
        {
            float LowDistance = PreviousDistance;
            float HighDistance = CurrentDistance;
            const bool bLowAir = bPreviousAir;
            for (int32 Iteration = 0; Iteration < BisectionIterations; ++Iteration)
            {
                const float MidDistance = 0.5f * (LowDistance + HighDistance);
                const FVector MidPoint = Origin + Direction * MidDistance;
                const float MidDensity = Generator.GetDensityAt(
                    MidPoint.X, MidPoint.Y, MidPoint.Z);
                if (!FMath::IsFinite(MidDensity))
                {
                    return false;
                }
                if ((MidDensity > 0.0f) == bLowAir)
                {
                    LowDistance = MidDistance;
                }
                else
                {
                    HighDistance = MidDistance;
                }
            }

            const float HitDistance = 0.5f * (LowDistance + HighDistance);
            OutHitPoint = Origin + Direction * HitDistance;
            const float NormalStep = 0.5f;
            const float DXP = Generator.GetDensityAt(
                OutHitPoint.X + NormalStep, OutHitPoint.Y, OutHitPoint.Z);
            const float DXN = Generator.GetDensityAt(
                OutHitPoint.X - NormalStep, OutHitPoint.Y, OutHitPoint.Z);
            const float DYP = Generator.GetDensityAt(
                OutHitPoint.X, OutHitPoint.Y + NormalStep, OutHitPoint.Z);
            const float DYN = Generator.GetDensityAt(
                OutHitPoint.X, OutHitPoint.Y - NormalStep, OutHitPoint.Z);
            const float DZP = Generator.GetDensityAt(
                OutHitPoint.X, OutHitPoint.Y, OutHitPoint.Z + NormalStep);
            const float DZN = Generator.GetDensityAt(
                OutHitPoint.X, OutHitPoint.Y, OutHitPoint.Z - NormalStep);
            if (!FMath::IsFinite(DXP) || !FMath::IsFinite(DXN)
                || !FMath::IsFinite(DYP) || !FMath::IsFinite(DYN)
                || !FMath::IsFinite(DZP) || !FMath::IsFinite(DZN))
            {
                return false;
            }
            OutNormal = FVector(DXP - DXN, DYP - DYN, DZP - DZN).GetSafeNormal();
            if (OutNormal.IsNearlyZero())
            {
                OutNormal = FVector::UpVector;
            }
            return true;
        }

        PreviousDistance = CurrentDistance;
        PreviousDensity = CurrentDensity;
    }
    return false;
}

void PutPixel(TArray<FColor>& Pixels, int32 Width, int32 Height, int32 X, int32 Y, const FColor& Color)
{
    if (X >= 0 && X < Width && Y >= 0 && Y < Height)
    {
        Pixels[X + Width * Y] = Color;
    }
}

void DrawLine(
    TArray<FColor>& Pixels,
    int32 Width,
    int32 Height,
    int32 X0,
    int32 Y0,
    int32 X1,
    int32 Y1,
    const FColor& Color)
{
    int32 DX = FMath::Abs(X1 - X0);
    const int32 SX = X0 < X1 ? 1 : -1;
    int32 DY = -FMath::Abs(Y1 - Y0);
    const int32 SY = Y0 < Y1 ? 1 : -1;
    int32 Error = DX + DY;
    for (;;)
    {
        PutPixel(Pixels, Width, Height, X0, Y0, Color);
        if (X0 == X1 && Y0 == Y1)
        {
            break;
        }
        const int32 DoubleError = 2 * Error;
        if (DoubleError >= DY)
        {
            Error += DY;
            X0 += SX;
        }
        if (DoubleError <= DX)
        {
            Error += DX;
            Y0 += SY;
        }
    }
}

bool ProjectPoint(
    const FVector& Point,
    const FVector& CameraPosition,
    const FExploreCameraBasis& Basis,
    int32 Width,
    int32 Height,
    FIntPoint& OutPixel)
{
    const FVector Relative = Point - CameraPosition;
    const float Depth = FVector::DotProduct(Relative, Basis.Forward);
    if (Depth <= KINDA_SMALL_NUMBER)
    {
        return false;
    }
    const float NdcX = FVector::DotProduct(Relative, Basis.Right)
        / (Depth * Basis.Aspect * Basis.TanHalfFov);
    const float NdcY = FVector::DotProduct(Relative, Basis.Up)
        / (Depth * Basis.TanHalfFov);
    OutPixel.X = FMath::RoundToInt((NdcX * 0.5f + 0.5f) * static_cast<float>(Width - 1));
    OutPixel.Y = FMath::RoundToInt((1.0f - (NdcY * 0.5f + 0.5f))
        * static_cast<float>(Height - 1));
    return NdcX >= -1.0f && NdcX <= 1.0f && NdcY >= -1.0f && NdcY <= 1.0f;
}

void DrawScaleMarker(
    TArray<FColor>& Pixels,
    int32 Width,
    int32 Height,
    const FVector& CameraPosition,
    const FVector& Target,
    const FExploreCameraBasis& Basis,
    bool& bOutProjected)
{
    constexpr float HumanHeightVoxels = 2.0f
        * FVoxelPlayerCapsuleConstants::HalfHeightVoxels;
    const FVector MarkerBase = FVector(
        Target.X,
        Target.Y,
        Target.Z - FVoxelPlayerCapsuleConstants::HalfHeightVoxels)
        - Basis.Right * 18.0f
        + Basis.Forward * 48.0f;
    const FVector MarkerTop = MarkerBase + FVector(0.0f, 0.0f, HumanHeightVoxels);
    FIntPoint BasePixel = FIntPoint::ZeroValue;
    FIntPoint TopPixel = FIntPoint::ZeroValue;
    bOutProjected = ProjectPoint(
        MarkerBase, CameraPosition, Basis, Width, Height, BasePixel)
        && ProjectPoint(MarkerTop, CameraPosition, Basis, Width, Height, TopPixel);
    if (!bOutProjected)
    {
        // The world-space marker is normally in frame by construction. Keep a visible fallback
        // for unusual camera/FOV overrides, while the JSON records that it was not projected.
        BasePixel = FIntPoint(12, Height - 12);
        TopPixel = FIntPoint(12, FMath::Max(12, Height - 12 - Height / 5));
    }

    const FColor MarkerColor(255, 220, 32, 255);
    DrawLine(Pixels, Width, Height, BasePixel.X, BasePixel.Y, TopPixel.X, TopPixel.Y, MarkerColor);
    const int32 TickHalfWidth = 5;
    for (const FIntPoint& Tick : { BasePixel, TopPixel })
    {
        DrawLine(
            Pixels,
            Width,
            Height,
            Tick.X - TickHalfWidth,
            Tick.Y,
            Tick.X + TickHalfWidth,
            Tick.Y,
            MarkerColor);
    }
}

bool SavePng(
    const FString& FilePath,
    const TArray<FColor>& Pixels,
    int32 Width,
    int32 Height,
    FString& OutError)
{
    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(
        FName(TEXT("ImageWrapper")));
    TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(
        EImageFormat::PNG);
    if (!ImageWrapper.IsValid()
        || !ImageWrapper->SetRaw(
            Pixels.GetData(),
            static_cast<int64>(Pixels.Num()) * sizeof(FColor),
            Width,
            Height,
            ERGBFormat::BGRA,
            8))
    {
        OutError = TEXT("ImageWrapper could not accept the BGRA render buffer.");
        return false;
    }
    const TArray64<uint8>& Compressed = ImageWrapper->GetCompressed(100);
    if (!FFileHelper::SaveArrayToFile(Compressed, *FilePath))
    {
        OutError = FString::Printf(TEXT("Could not write PNG '%s'."), *FilePath);
        return false;
    }
    return true;
}

bool RunRender(
    const FExploreArguments& Arguments,
    const FExploreWorld& World,
    const FExploreWalkOutput& CameraSeedWalk,
    FExploreRenderOutput& OutOutput)
{
    OutOutput = FExploreRenderOutput();
    OutOutput.Width = Arguments.RenderWidth;
    OutOutput.Height = Arguments.RenderHeight;
    OutOutput.FixedStepVoxels = Arguments.RenderStepVoxels;
    OutOutput.FixedStepMeters = Arguments.RenderStepVoxels
        * FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
    OutOutput.BisectionIterations = 10;
    OutOutput.MaxDistanceVoxels = Arguments.RenderMaxDistanceVoxels;
    OutOutput.EstimatedFixedDensitySamples = static_cast<int64>(Arguments.RenderWidth)
        * static_cast<int64>(Arguments.RenderHeight)
        * 3ll
        * static_cast<int64>(FMath::Max(
            1,
            FMath::CeilToInt(
                Arguments.RenderMaxDistanceVoxels / Arguments.RenderStepVoxels)));
    OutOutput.StepRationale = TEXT(
        "Density has no signed-distance/Lipschitz contract. The renderer therefore advances at a "
        "fixed 0.25-voxel default step (override with -renderstep), detects density sign crossings, "
        "and refines each crossing with 10 bisection iterations; it never sphere-traces.");

    OutOutput.CameraSeedPolicy = TEXT(
        "Every camera origin is a density-checked pose from the exact player-fit walk mask: "
        "arrival, departure, last reached, and agent-final poses are considered in fixed order; "
        "duplicate poses are removed. The target is offset toward the fitted route so the view is "
        "from inside the walkable space.");

    TArray<TPair<FVector, FString>, TInlineAllocator<4>> CameraSeeds;
    const auto AddCameraSeed = [&](bool bAvailable, const FVector& Position, const TCHAR* Source)
    {
        if (!bAvailable
            || !FMath::IsFinite(Position.X)
            || !FMath::IsFinite(Position.Y)
            || !FMath::IsFinite(Position.Z))
        {
            return;
        }

        // The walk mask is authoritative, but this guard catches stale/corrupt report data before
        // a render can silently start inside rock.
        const float Density = World.Generator->GetDensityAt(Position.X, Position.Y, Position.Z);
        if (!FMath::IsFinite(Density) || !(Density > 0.0f))
        {
            return;
        }
        for (const TPair<FVector, FString>& Existing : CameraSeeds)
        {
            if (Existing.Key.Equals(Position, 0.001f))
            {
                return;
            }
        }
        CameraSeeds.Emplace(Position, FString(Source));
    };

    AddCameraSeed(
        CameraSeedWalk.bHasArrival,
        CameraSeedWalk.ArrivalVoxels,
        TEXT("arrival_player_fit"));
    AddCameraSeed(
        CameraSeedWalk.bHasDeparture,
        CameraSeedWalk.DepartureVoxels,
        TEXT("departure_player_fit"));
    AddCameraSeed(
        CameraSeedWalk.Report.bHasLastReachedPosition,
        CameraSeedWalk.Report.LastReachedVoxels,
        TEXT("last_reached_player_fit"));
    AddCameraSeed(
        CameraSeedWalk.Report.bHasAgentFinalPosition,
        CameraSeedWalk.Report.AgentFinalVoxels,
        TEXT("agent_final_player_fit"));

    if (CameraSeeds.Num() == 0)
    {
        OutOutput.Status = TEXT("refused");
        OutOutput.RefusalReason = TEXT(
            "The player-fit seed pass produced no density-positive pose; refusing an inside-rock camera.");
        return false;
    }

    OutOutput.CameraSeedCount = CameraSeeds.Num();
    OutOutput.bAllCamerasPlayerFit = true;
    FVector RouteFocus = FVector::ZeroVector;
    int32 RouteFocusCount = 0;
    if (CameraSeedWalk.bHasArrival)
    {
        RouteFocus += CameraSeedWalk.ArrivalVoxels;
        ++RouteFocusCount;
    }
    if (CameraSeedWalk.bHasDeparture)
    {
        RouteFocus += CameraSeedWalk.DepartureVoxels;
        ++RouteFocusCount;
    }
    if (RouteFocusCount > 0)
    {
        RouteFocus /= static_cast<float>(RouteFocusCount);
    }
    else
    {
        RouteFocus = CameraSeeds[0].Key;
    }

    const FVector KeyLightDirection = FVector(-0.35f, -0.45f, 0.82f).GetSafeNormal();
    const FVector FillLightDirection = FVector(0.65f, 0.30f, 0.70f).GetSafeNormal();
    constexpr float AmbientFill = 0.62f;
    constexpr float KeyLightStrength = 0.50f;
    constexpr float FillLightStrength = 0.24f;

    for (int32 ViewIndex = 0; ViewIndex < CameraSeeds.Num(); ++ViewIndex)
    {
        const FVector CameraSeedPose = CameraSeeds[ViewIndex].Key;
        const FVector CameraPosition = CameraSeedPose
            + FVector::UpVector * (FVoxelPlayerCapsuleConstants::HalfHeightVoxels - 0.5f);
        const float CameraDensity = World.Generator->GetDensityAt(
            CameraPosition.X, CameraPosition.Y, CameraPosition.Z);
        if (!FMath::IsFinite(CameraDensity) || !(CameraDensity > 0.0f))
        {
            OutOutput.Status = TEXT("refused");
            OutOutput.RefusalReason = TEXT(
                "The eye-height offset of a player-fit camera entered solid density.");
            return false;
        }
        FVector LookDirection = RouteFocus - CameraPosition;
        LookDirection.Z = 0.0f;
        LookDirection.Normalize();
        if (LookDirection.IsNearlyZero())
        {
            LookDirection = FVector(
                ViewIndex == 1 ? -0.6f : 0.8f,
                ViewIndex == 2 ? 0.7f : -0.4f,
                0.18f).GetSafeNormal();
        }
        // Keep the camera at the fitted pose, while aiming at roughly eye level in the route.
        const FVector Target = CameraPosition + LookDirection * 96.0f + FVector::UpVector * 8.0f;
        FExploreCameraBasis Basis;
        if (!BuildCameraBasis(
                CameraPosition,
                Target,
                70.0f,
                Arguments.RenderWidth,
                Arguments.RenderHeight,
                Basis))
        {
            OutOutput.Status = TEXT("error");
            OutOutput.RefusalReason = TEXT("Could not construct a render camera basis.");
            return false;
        }

        TArray<FColor> Pixels;
        Pixels.Init(FColor(12, 18, 28, 255), Arguments.RenderWidth * Arguments.RenderHeight);
        int32 HitPixels = 0;
        for (int32 PixelY = 0; PixelY < Arguments.RenderHeight; ++PixelY)
        {
            for (int32 PixelX = 0; PixelX < Arguments.RenderWidth; ++PixelX)
            {
                const FVector RayDirection = MakeRayDirection(
                    Basis,
                    PixelX,
                    PixelY,
                    Arguments.RenderWidth,
                    Arguments.RenderHeight);
                FVector HitPoint;
                FVector Normal;
                if (!TraceDensityRay(
                        *World.Generator,
                        CameraPosition,
                        RayDirection,
                        Arguments.RenderStepVoxels,
                        Arguments.RenderMaxDistanceVoxels,
                        OutOutput.BisectionIterations,
                        HitPoint,
                        Normal))
                {
                    continue;
                }

                ++HitPixels;
                const float Key = FMath::Max(
                    0.0f, FVector::DotProduct(Normal, KeyLightDirection));
                const float Fill = FMath::Max(
                    0.0f, FVector::DotProduct(Normal, FillLightDirection));
                const float Brightness = FMath::Clamp(
                    AmbientFill + KeyLightStrength * Key + FillLightStrength * Fill,
                    0.0f, 1.25f);
                const uint8 RockR = static_cast<uint8>(FMath::Clamp(100.0f * Brightness, 0.0f, 255.0f));
                const uint8 RockG = static_cast<uint8>(FMath::Clamp(128.0f * Brightness, 0.0f, 255.0f));
                const uint8 RockB = static_cast<uint8>(FMath::Clamp(160.0f * Brightness, 0.0f, 255.0f));
                PutPixel(
                    Pixels,
                    Arguments.RenderWidth,
                    Arguments.RenderHeight,
                    PixelX,
                    PixelY,
                    FColor(RockR, RockG, RockB, 255));
            }
        }

        bool bScaleMarkerProjected = false;
        DrawScaleMarker(
            Pixels,
            Arguments.RenderWidth,
            Arguments.RenderHeight,
            CameraPosition,
            Target,
            Basis,
            bScaleMarkerProjected);

        FExploreRenderFrame& Frame = OutOutput.Frames.AddDefaulted_GetRef();
        Frame.FileName = FString::Printf(TEXT("render_%02d.png"), ViewIndex);
        Frame.Purpose = CameraSeeds[ViewIndex].Value;
        Frame.CameraSeedSource = CameraSeeds[ViewIndex].Value;
        Frame.CameraSeedPoseVoxels = CameraSeedPose;
        Frame.CameraVoxels = CameraPosition;
        Frame.TargetVoxels = Target;
        Frame.HitPixels = HitPixels;
        Frame.bScaleMarkerProjected = bScaleMarkerProjected;

        FString Error;
        if (!SavePng(
                FPaths::Combine(Arguments.OutDirectory, Frame.FileName),
                Pixels,
                Arguments.RenderWidth,
                Arguments.RenderHeight,
                Error))
        {
            OutOutput.Status = TEXT("error");
            OutOutput.RefusalReason = Error;
            return false;
        }
    }

    OutOutput.Status = TEXT("ok");
    return true;
}

void DrawPointMarker(
    TArray<FColor>& Pixels,
    int32 Width,
    int32 Height,
    const FVector& Point,
    const FVector& CameraPosition,
    const FExploreCameraBasis& Basis,
    const FColor& Color,
    bool& bOutProjected)
{
    FIntPoint Pixel = FIntPoint::ZeroValue;
    bOutProjected = ProjectPoint(
        Point, CameraPosition, Basis, Width, Height, Pixel);
    if (!bOutProjected)
    {
        return;
    }

    constexpr int32 MarkerRadius = 7;
    DrawLine(
        Pixels, Width, Height,
        Pixel.X - MarkerRadius, Pixel.Y,
        Pixel.X + MarkerRadius, Pixel.Y,
        Color);
    DrawLine(
        Pixels, Width, Height,
        Pixel.X, Pixel.Y - MarkerRadius,
        Pixel.X, Pixel.Y + MarkerRadius,
        Color);
    DrawLine(
        Pixels, Width, Height,
        Pixel.X - 4, Pixel.Y - 4,
        Pixel.X + 4, Pixel.Y + 4,
        Color);
    DrawLine(
        Pixels, Width, Height,
        Pixel.X - 4, Pixel.Y + 4,
        Pixel.X + 4, Pixel.Y - 4,
        Color);
}

bool RunFailureBoundaryRender(
    const FExploreArguments& Arguments,
    const FExploreWorld& World,
    const FExploreWalkOutput& Walk,
    FExploreRenderOutput& InOutOutput)
{
    const FVoxelPlayerFitWalkReport& Report = Walk.Report;
    if (!Report.bHasAgentFinalPosition || !Report.bHasTargetComponentNearestCell)
    {
        return false;
    }

    const FVector AgentFinal = Report.AgentFinalVoxels;
    const FVector TargetComponentCell = Report.TargetComponentNearestVoxels;
    const FVector LastReached = Report.LastReachedVoxels;
    const bool bHaveLastReached = Report.bHasLastReachedPosition;
    const FVector Focus = (AgentFinal + TargetComponentCell) * 0.5f;
    const FVector Segment = TargetComponentCell - AgentFinal;
    const FVector SegmentDirection = Segment.GetSafeNormal();
    if (SegmentDirection.IsNearlyZero())
    {
        return false;
    }
    FVector ViewSide = FVector::CrossProduct(SegmentDirection, FVector::UpVector).GetSafeNormal();
    if (ViewSide.IsNearlyZero())
    {
        ViewSide = FVector::CrossProduct(SegmentDirection, FVector::RightVector).GetSafeNormal();
    }
    const float SegmentLength = Segment.Size();
    const float ViewSideOffset = FMath::Min(64.0f, FMath::Max(16.0f, 0.25f * SegmentLength));
    // Failure-focus views are still required to originate from the fitted graph. The old
    // implementation nudged the camera four voxels toward/away from the boundary and could put
    // the viewpoint in the very rock that this diagnostic is meant to explain.
    const FVector CameraCandidates[] = {
        AgentFinal,
        bHaveLastReached ? LastReached : AgentFinal,
    };
    const FVector ViewTargets[] = {
        Focus + ViewSide * ViewSideOffset,
        Focus - ViewSide * ViewSideOffset,
    };

    for (int32 ViewIndex = 0; ViewIndex < UE_ARRAY_COUNT(ViewTargets); ++ViewIndex)
    {
        const FVector CameraSeedPose = CameraCandidates[ViewIndex];
        const FVector CameraPosition = CameraSeedPose
            + FVector::UpVector * (FVoxelPlayerCapsuleConstants::HalfHeightVoxels - 0.5f);
        const float CameraDensity = World.Generator->GetDensityAt(
            CameraPosition.X, CameraPosition.Y, CameraPosition.Z);
        if (!FMath::IsFinite(CameraDensity) || !(CameraDensity > 0.0f))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[VoxelForgeExplore] refusing failure-focus camera %d: fitted pose is not air."),
                ViewIndex);
            return false;
        }
        const float FailureMaxDistanceVoxels = FMath::Max(
            Arguments.RenderMaxDistanceVoxels,
            FMath::Max(
                FVector::Dist(CameraPosition, AgentFinal),
                FVector::Dist(CameraPosition, TargetComponentCell)) + 32.0f);
        FExploreCameraBasis Basis;
        if (!BuildCameraBasis(
                CameraPosition,
                ViewTargets[ViewIndex],
                70.0f,
                Arguments.RenderWidth,
                Arguments.RenderHeight,
                Basis))
        {
            return false;
        }

        TArray<FColor> Pixels;
        Pixels.Init(FColor(12, 18, 28, 255), Arguments.RenderWidth * Arguments.RenderHeight);
        int32 HitPixels = 0;
        const FVector KeyLightDirection = FVector(-0.35f, -0.45f, 0.82f).GetSafeNormal();
        const FVector FillLightDirection = FVector(0.65f, 0.30f, 0.70f).GetSafeNormal();
        constexpr float AmbientFill = 0.62f;
        constexpr float KeyLightStrength = 0.50f;
        constexpr float FillLightStrength = 0.24f;
        for (int32 PixelY = 0; PixelY < Arguments.RenderHeight; ++PixelY)
        {
            for (int32 PixelX = 0; PixelX < Arguments.RenderWidth; ++PixelX)
            {
                const FVector RayDirection = MakeRayDirection(
                    Basis,
                    PixelX,
                    PixelY,
                    Arguments.RenderWidth,
                    Arguments.RenderHeight);
                FVector HitPoint;
                FVector Normal;
                if (!TraceDensityRay(
                    *World.Generator,
                    CameraPosition,
                    RayDirection,
                    Arguments.RenderStepVoxels,
                    FailureMaxDistanceVoxels,
                    InOutOutput.BisectionIterations,
                    HitPoint,
                    Normal))
                {
                    continue;
                }

                ++HitPixels;
                const float Key = FMath::Max(
                    0.0f, FVector::DotProduct(Normal, KeyLightDirection));
                const float Fill = FMath::Max(
                    0.0f, FVector::DotProduct(Normal, FillLightDirection));
                const float Brightness = FMath::Clamp(
                    AmbientFill + KeyLightStrength * Key + FillLightStrength * Fill,
                    0.0f, 1.25f);
                const uint8 RockR = static_cast<uint8>(FMath::Clamp(100.0f * Brightness, 0.0f, 255.0f));
                const uint8 RockG = static_cast<uint8>(FMath::Clamp(128.0f * Brightness, 0.0f, 255.0f));
                const uint8 RockB = static_cast<uint8>(FMath::Clamp(160.0f * Brightness, 0.0f, 255.0f));
                PutPixel(
                    Pixels,
                    Arguments.RenderWidth,
                    Arguments.RenderHeight,
                    PixelX,
                    PixelY,
                    FColor(RockR, RockG, RockB, 255));
            }
        }

        bool bAgentProjected = false;
        bool bTargetProjected = false;
        bool bLastReachedProjected = false;
        DrawPointMarker(
            Pixels,
            Arguments.RenderWidth,
            Arguments.RenderHeight,
            AgentFinal,
            CameraPosition,
            Basis,
            FColor(242, 72, 72, 255),
            bAgentProjected);
        DrawPointMarker(
            Pixels,
            Arguments.RenderWidth,
            Arguments.RenderHeight,
            TargetComponentCell,
            CameraPosition,
            Basis,
            FColor(72, 242, 112, 255),
            bTargetProjected);
        if (bHaveLastReached)
        {
            DrawPointMarker(
                Pixels,
                Arguments.RenderWidth,
                Arguments.RenderHeight,
                LastReached,
                CameraPosition,
                Basis,
                FColor(242, 176, 56, 255),
                bLastReachedProjected);
        }

        FIntPoint AgentPixel = FIntPoint::ZeroValue;
        FIntPoint TargetPixel = FIntPoint::ZeroValue;
        if (ProjectPoint(
                AgentFinal,
                CameraPosition,
                Basis,
                Arguments.RenderWidth,
                Arguments.RenderHeight,
                AgentPixel)
            && ProjectPoint(
                TargetComponentCell,
                CameraPosition,
                Basis,
                Arguments.RenderWidth,
                Arguments.RenderHeight,
                TargetPixel))
        {
            DrawLine(
                Pixels,
                Arguments.RenderWidth,
                Arguments.RenderHeight,
                AgentPixel.X,
                AgentPixel.Y,
                TargetPixel.X,
                TargetPixel.Y,
                FColor(244, 214, 72, 255));
        }

        FExploreRenderFrame& Frame = InOutOutput.Frames.AddDefaulted_GetRef();
        Frame.FileName = FString::Printf(TEXT("failure_boundary_%02d.png"), ViewIndex);
        Frame.Purpose = TEXT("agent_final_red_target_component_nearest_green_last_reached_orange");
        Frame.CameraSeedSource = bHaveLastReached && ViewIndex == 1
            ? TEXT("last_reached_player_fit")
            : TEXT("agent_final_player_fit");
        Frame.CameraSeedPoseVoxels = CameraSeedPose;
        Frame.CameraVoxels = CameraPosition;
        Frame.TargetVoxels = ViewTargets[ViewIndex];
        Frame.HitPixels = HitPixels;
        Frame.bFailureMarkersProjected = bAgentProjected && bTargetProjected
            && (!bHaveLastReached || bLastReachedProjected);

        FString Error;
        if (!SavePng(
                FPaths::Combine(Arguments.OutDirectory, Frame.FileName),
                Pixels,
                Arguments.RenderWidth,
                Arguments.RenderHeight,
                Error))
        {
            InOutOutput.Status = TEXT("error");
            InOutOutput.RefusalReason = Error;
            return false;
        }
    }

    return true;
}

bool RunWalk(
    const FExploreArguments& Arguments,
    const FExploreWorld& World,
    FExploreWalkOutput& OutOutput)
{
    OutOutput = FExploreWalkOutput();
    OutOutput.WindowPolicy = NeedsOriginInWindow(Arguments.Archetype)
        ? TEXT("origin-inclusive target-strate passage-envelope AABB + margin")
        : TEXT("target-strate passage-envelope AABB + margin");

    const int64 EstimatedWorkingBytes = static_cast<int64>(Arguments.MaxWalkCells)
        * EstimatedWalkBytesPerCell;
    if (EstimatedWorkingBytes > MaxWalkWorkingBytes)
    {
        OutOutput.Status = TEXT("refused");
        OutOutput.RefusalReason = FString::Printf(
            TEXT("Refused before sampling: estimated walk memory %lld bytes exceeds cap %lld bytes."),
            static_cast<long long>(EstimatedWorkingBytes),
            static_cast<long long>(MaxWalkWorkingBytes));
        return false;
    }

    const FVoxelPassage* ArrivalPassage = nullptr;
    const FVoxelPassage* DeparturePassage = nullptr;
    if (!FindMouths(
            *World.Manager,
            Arguments.Slot,
            OutOutput.ArrivalVoxels,
            OutOutput.DepartureVoxels,
            OutOutput.ArrivalPassageCount,
            OutOutput.DeparturePassageCount,
            ArrivalPassage,
            DeparturePassage))
    {
        OutOutput.Status = TEXT("refused");
        OutOutput.RefusalReason = TEXT(
            "The target slot did not have both one-step chain mouths in the generated passage list.");
        return false;
    }
    OutOutput.bHasArrival = true;
    OutOutput.bHasDeparture = true;

    // Temporary seed-14 probe while diagnosing the fitted VerticalShafts junction. This is
    // intentionally narrow and will be removed after the floor/tree seam is corrected.
    if (Arguments.Archetype == ECaveGeneratorType::VerticalShafts && Arguments.Seed == 14)
    {
        const FVector ProbeA = OutOutput.ArrivalVoxels;
        const FVector ProbeB(-100.8f, -26.9f, ProbeA.Z);
        for (int32 Index = 0; Index <= 10; ++Index)
        {
            const float T = static_cast<float>(Index) / 10.0f;
            const FVector P = FMath::Lerp(ProbeA, ProbeB, T);
            const float Dm704 = World.Generator->GetDensityAt(P.X, P.Y, -704.0f);
            const float Dm702 = World.Generator->GetDensityAt(P.X, P.Y, -702.0f);
            const float Dm7005 = World.Generator->GetDensityAt(P.X, P.Y, -700.5f);
            const float Dm700 = World.Generator->GetDensityAt(P.X, P.Y, -700.0f);
            const float Dm6995 = World.Generator->GetDensityAt(P.X, P.Y, -699.5f);
            const float Dm697 = World.Generator->GetDensityAt(P.X, P.Y, -697.0f);
            float MinBody = FLT_MAX;
            int32 NumSolidBody = 0;
            int32 SolidOffsetX = 0;
            int32 SolidOffsetY = 0;
            int32 SolidBodyRow = -1;
            for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
            {
                for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
                {
                    for (int32 BodyRow = 0; BodyRow < 8; ++BodyRow)
                    {
                        const float BodyDensity = World.Generator->GetDensityAt(
                            P.X + static_cast<float>(OffsetX),
                            P.Y + static_cast<float>(OffsetY),
                            -699.5f + static_cast<float>(BodyRow));
                        if (BodyDensity < MinBody)
                        {
                            MinBody = BodyDensity;
                            SolidOffsetX = OffsetX;
                            SolidOffsetY = OffsetY;
                            SolidBodyRow = BodyRow;
                        }
                        NumSolidBody += BodyDensity <= 0.0f ? 1 : 0;
                    }
                }
            }
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeExplore][TemporaryShaftProbe] t=%.2f xy=(%.2f,%.2f) "
                     "d[-704,-702,-700.5,-700,-699.5,-697]=[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f] "
                     "bodyMin=%.3f bodySolid=%d/72 firstMinOffset=(%d,%d) row=%d"),
                T, P.X, P.Y, Dm704, Dm702, Dm7005, Dm700, Dm6995, Dm697,
                MinBody, NumSolidBody, SolidOffsetX, SolidOffsetY, SolidBodyRow);
        }
    }

    FVoxelStrateMeasureSettings Settings;
    Settings.SampleStep = 1;
    Settings.MaxCells = Arguments.MaxWalkCells;
    Settings.MaxRouteRetries = 16;
    Settings.CoverPointA = FVector2D(OutOutput.ArrivalVoxels.X, OutOutput.ArrivalVoxels.Y);
    Settings.CoverPointB = FVector2D(OutOutput.DepartureVoxels.X, OutOutput.DepartureVoxels.Y);
    Settings.CoverMarginVoxels = FMath::CeilToFloat(
        Settings.PlayerCapsuleRadiusVoxels) + 2.0f;

    // A mouth-AABB is not a landing window: the landing door and its local apron can legitimately
    // leave that box before the player reaches the next room. Expand the same two-point box just
    // enough to contain the selected production mouth envelope. This is still a bounded,
    // deterministic measurement window; the central inter-strate descent belongs to the adjacent
    // strata and is not folded into this intra-strate walk question.
    const float WindowMinX = FMath::Min(OutOutput.ArrivalVoxels.X, OutOutput.DepartureVoxels.X);
    const float WindowMaxX = FMath::Max(OutOutput.ArrivalVoxels.X, OutOutput.DepartureVoxels.X);
    const float WindowMinY = FMath::Min(OutOutput.ArrivalVoxels.Y, OutOutput.DepartureVoxels.Y);
    const float WindowMaxY = FMath::Max(OutOutput.ArrivalVoxels.Y, OutOutput.DepartureVoxels.Y);
    const auto IncludeTargetPassageEnvelope = [&](
        const FVoxelPassage* Passage,
        const FVoxelPassageLanding& Landing)
    {
        if (Passage == nullptr)
        {
            return;
        }

        const auto IncludeXY = [&](const FVector& Point, float Radius)
        {
            if (!FMath::IsFinite(Point.X) || !FMath::IsFinite(Point.Y)
                || !FMath::IsFinite(Point.Z) || !FMath::IsFinite(Radius))
            {
                return;
            }
            const float AbsRadius = FMath::Abs(Radius);
            // Include control points whose centreline belongs to the measured strate. A control
            // point just outside the boundary belongs to the adjacent strate's descent; pulling
            // that whole switchback into this bounded walk box can multiply the sampled volume
            // without improving the intra-strate mouth-to-mouth question.
            if (Point.Z < static_cast<float>(World.TargetBottomWorldZ)
                || Point.Z > static_cast<float>(World.TargetTopWorldZ))
            {
                return;
            }
            const float ExcessX = FMath::Max(
                0.0f,
                FMath::Max(
                    WindowMinX - (Point.X + AbsRadius),
                    (Point.X - AbsRadius) - WindowMaxX));
            const float ExcessY = FMath::Max(
                0.0f,
                FMath::Max(
                    WindowMinY - (Point.Y + AbsRadius),
                    (Point.Y - AbsRadius) - WindowMaxY));
            Settings.CoverMarginVoxels = FMath::Max(
                Settings.CoverMarginVoxels,
                FMath::Max(ExcessX, ExcessY));
        };

        IncludeXY(
            Landing.StandingPoint,
            Landing.HalfWidth + VoxelPassageGeometry::LandingCarveBlendVoxels);
        // The first/last three points are the production mouth, apron, and first slope section.
        // Do not pull the whole inter-strate switchback into the bounded target-strate test.
        const int32 NumLocalPoints = FMath::Min(3, Passage->ControlPoints.Num());
        const bool bUseUpperEnd = &Landing == &Passage->UpperLanding;
        const int32 FirstPoint = bUseUpperEnd
            ? 0 : FMath::Max(0, Passage->ControlPoints.Num() - NumLocalPoints);
        const int32 LastPoint = bUseUpperEnd
            ? NumLocalPoints : Passage->ControlPoints.Num();
        for (int32 PointIndex = FirstPoint; PointIndex < LastPoint; ++PointIndex)
        {
            const float Radius = Passage->ControlRadii.IsValidIndex(PointIndex)
                ? Passage->ControlRadii[PointIndex] : Passage->Radius;
            IncludeXY(Passage->ControlPoints[PointIndex], Radius
                + VoxelPassageGeometry::LandingCarveBlendVoxels);
        }
    };
    IncludeTargetPassageEnvelope(
        ArrivalPassage,
        ArrivalPassage != nullptr ? ArrivalPassage->LowerLanding : FVoxelPassageLanding());
    IncludeTargetPassageEnvelope(
        DeparturePassage,
        DeparturePassage != nullptr ? DeparturePassage->UpperLanding : FVoxelPassageLanding());
    Settings.bIncludeOriginInCoverWindow = NeedsOriginInWindow(Arguments.Archetype);

    TArray<FVector> LandingRoomProbePoints;

    auto AppendLandingRoomProbes = [&LandingRoomProbePoints](
        const FVoxelPassageLanding& Landing)
    {
        const float ProbeOffset = FMath::Min(
            2.0f,
            FMath::Max(0.0f, Landing.HalfWidth - 2.0f));
        LandingRoomProbePoints.Add(Landing.StandingPoint);
        LandingRoomProbePoints.Add(
            Landing.StandingPoint + FVector(ProbeOffset, 0.0f, 0.0f));
        LandingRoomProbePoints.Add(
            Landing.StandingPoint + FVector(-ProbeOffset, 0.0f, 0.0f));
        LandingRoomProbePoints.Add(
            Landing.StandingPoint + FVector(0.0f, ProbeOffset, 0.0f));
        LandingRoomProbePoints.Add(
            Landing.StandingPoint + FVector(0.0f, -ProbeOffset, 0.0f));
    };
    if (ArrivalPassage != nullptr)
    {
        AppendLandingRoomProbes(ArrivalPassage->LowerLanding);
    }
    if (DeparturePassage != nullptr)
    {
        AppendLandingRoomProbes(DeparturePassage->UpperLanding);
    }

    const FGeneratorDensitySampler Sampler(*World.Generator);
    if (!VF_MeasurePlayerFitWalkWithSampler(
            Sampler,
            World.TargetBottomWorldZ,
            World.TargetTopWorldZ,
            World.TargetBoundarySealThickness,
            OutOutput.ArrivalVoxels,
            OutOutput.DepartureVoxels,
            Settings,
            OutOutput.Report,
            &LandingRoomProbePoints))
    {
        OutOutput.Status = TEXT("refused");
        OutOutput.RefusalReason = OutOutput.Report.RefusalReason;
        return false;
    }

    // Part C is a separate observation window.  Keep the Part A mouth-sized result untouched,
    // then repeat the same exact fit walk with the route window expanded to include (0,0).  This
    // is measurement-only: it changes neither the density field nor any generation parameter.
    OutOutput.bOriginCheckAttempted = true;
    FVoxelStrateMeasureSettings OriginSettings = Settings;
    OriginSettings.bIncludeOriginInCoverWindow = true;
    OriginSettings.bForceOriginColumnInCoverWindow = true;
    if (VF_MeasurePlayerFitWalkWithSampler(
            Sampler,
            World.TargetBottomWorldZ,
            World.TargetTopWorldZ,
            World.TargetBoundarySealThickness,
            OutOutput.ArrivalVoxels,
            OutOutput.DepartureVoxels,
            OriginSettings,
            OutOutput.OriginCheckReport))
    {
        OutOutput.bOriginCheckAvailable = true;
    }
    else
    {
        OutOutput.OriginCheckRefusalReason = OutOutput.OriginCheckReport.RefusalReason;
    }
    const int32 ArrivalProbeCount = ArrivalPassage != nullptr ? 5 : 0;
    const int32 DepartureProbeCount = DeparturePassage != nullptr ? 5 : 0;
    const int32 LandingProbeCount = ArrivalProbeCount + DepartureProbeCount;
    OutOutput.ArrivalLandingRoomProbeCount = ArrivalProbeCount;
    OutOutput.DepartureLandingRoomProbeCount = DepartureProbeCount;
    for (int32 ProbeIndex = 0;
         ProbeIndex < FMath::Min(LandingProbeCount,
             OutOutput.Report.ComponentProbePlayerFit.Num());
         ++ProbeIndex)
    {
        const bool bPlayerFit = OutOutput.Report.ComponentProbePlayerFit[ProbeIndex] != 0u;
        const bool bInArrivalComponent =
            OutOutput.Report.ComponentProbeInStartComponent.IsValidIndex(ProbeIndex)
            && OutOutput.Report.ComponentProbeInStartComponent[ProbeIndex] != 0u;
        if (ProbeIndex < ArrivalProbeCount)
        {
            OutOutput.ArrivalLandingRoomPlayerFitProbeCount += bPlayerFit ? 1 : 0;
            OutOutput.ArrivalLandingRoomArrivalComponentProbeCount +=
                bInArrivalComponent ? 1 : 0;
        }
        else
        {
            OutOutput.DepartureLandingRoomPlayerFitProbeCount += bPlayerFit ? 1 : 0;
            OutOutput.DepartureLandingRoomArrivalComponentProbeCount +=
                bInArrivalComponent ? 1 : 0;
        }
    }
    OutOutput.Status = TEXT("ok");
    return true;
}

int64 EstimateExportWorkingBytes(int32 RegionSize)
{
    const int64 TileCells = static_cast<int64>(CHUNK_SIZE)
        * static_cast<int64>(CHUNK_SIZE)
        * static_cast<int64>(CHUNK_SIZE);
    const int64 TileGridSamples = static_cast<int64>(CHUNK_SIZE + 3)
        * static_cast<int64>(CHUNK_SIZE + 3)
        * static_cast<int64>(CHUNK_SIZE + 3);
    // Export streams one canonical tile at a time. This preflight covers one mesher tile's scalar
    // grid, edge map, mesh arrays, and temporary triangle lists; the region's OBJ is not retained
    // in memory. RegionSize is still accepted here to keep the call site's contract explicit.
    (void)RegionSize;
    return TileGridSamples * static_cast<int64>(sizeof(float)) + TileCells * 448ll;
}

bool WriteUtf8(FArchive& Archive, const FString& Text)
{
    FTCHARToUTF8 Converted(*Text);
    if (Converted.Length() > 0)
    {
        Archive.Serialize(const_cast<ANSICHAR*>(Converted.Get()), Converted.Length());
    }
    return !Archive.IsError();
}

bool ValidateCanonicalTile(const FVoxelMeshData& MeshData, FString& OutError)
{
    if (MeshData.Vertices.Num() != MeshData.Normals.Num()
        || MeshData.Vertices.Num() != MeshData.UVs.Num()
        || MeshData.Vertices.Num() != MeshData.Colors.Num()
        || MeshData.Triangles.Num() % 3 != 0)
    {
        OutError = TEXT("Canonical mesher returned non-parallel mesh arrays.");
        return false;
    }
    return true;
}

bool WriteObjHeader(FArchive& Archive)
{
    bool bOk = WriteUtf8(Archive, TEXT("# VoxelForge authoritative marching-cubes mesh\n"));
    bOk = bOk && WriteUtf8(Archive,
        TEXT("# Positions are metres; UVs retain canonical voxel-space values; source winding is preserved.\n"));
    bOk = bOk && WriteUtf8(Archive, TEXT("o VoxelForgeExplore\n"));
    return bOk && !Archive.IsError();
}

bool WriteObjTile(
    FArchive& Archive,
    const FVoxelMeshData& MeshData,
    int32 VertexOffset,
    FString& OutError)
{
    if (!ValidateCanonicalTile(MeshData, OutError))
    {
        return false;
    }
    bool bOk = true;
    for (const FVector& Vertex : MeshData.Vertices)
    {
        bOk = bOk && WriteUtf8(Archive, FString::Printf(
            TEXT("v %.9f %.9f %.9f\n"),
            Vertex.X / 100.0,
            Vertex.Y / 100.0,
            Vertex.Z / 100.0));
    }
    for (const FVector2D& UV : MeshData.UVs)
    {
        bOk = bOk && WriteUtf8(Archive, FString::Printf(
            TEXT("vt %.9f %.9f\n"), UV.X, UV.Y));
    }
    for (const FVector& Normal : MeshData.Normals)
    {
        bOk = bOk && WriteUtf8(Archive, FString::Printf(
            TEXT("vn %.9f %.9f %.9f\n"), Normal.X, Normal.Y, Normal.Z));
    }

    const int32 NumTriangles = MeshData.Triangles.Num() / 3;
    const int32 FirstCeilingTriangle = FMath::Clamp(
        NumTriangles - MeshData.NumCeilingTriangles, 0, NumTriangles);
    if (NumTriangles > 0)
    {
        bOk = bOk && WriteUtf8(Archive, TEXT("usemtl VoxelGround\n"));
    }
    for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
    {
        if (Triangle == FirstCeilingTriangle)
        {
            bOk = bOk && WriteUtf8(Archive, TEXT("usemtl VoxelCeiling\n"));
        }
        const int32 Base = Triangle * 3;
        const int32 SourceA = MeshData.Triangles[Base];
        const int32 SourceB = MeshData.Triangles[Base + 1];
        const int32 SourceC = MeshData.Triangles[Base + 2];
        if (SourceA < 0 || SourceB < 0 || SourceC < 0
            || SourceA >= MeshData.Vertices.Num()
            || SourceB >= MeshData.Vertices.Num()
            || SourceC >= MeshData.Vertices.Num())
        {
            OutError = TEXT("Canonical mesher returned an out-of-range triangle index.");
            return false;
        }
        const int32 A = SourceA + 1 + VertexOffset;
        const int32 B = SourceB + 1 + VertexOffset;
        const int32 C = SourceC + 1 + VertexOffset;
        bOk = bOk && WriteUtf8(Archive, FString::Printf(
            TEXT("f %d/%d/%d %d/%d/%d %d/%d/%d\n"),
            A, A, A, B, B, B, C, C, C));
    }
    if (!bOk || Archive.IsError())
    {
        OutError = TEXT("Could not write a canonical OBJ tile.");
        return false;
    }
    return true;
}

void WriteJsonVector(FExploreJsonWriter& Writer, const TCHAR* Key, const FVector& Value, float Scale)
{
    Writer.WriteObjectStart(Key);
    Writer.WriteValue(TEXT("x"), static_cast<double>(Value.X) * Scale);
    Writer.WriteValue(TEXT("y"), static_cast<double>(Value.Y) * Scale);
    Writer.WriteValue(TEXT("z"), static_cast<double>(Value.Z) * Scale);
    Writer.WriteObjectEnd();
}

void WriteJsonIntVector(FExploreJsonWriter& Writer, const TCHAR* Key, const FIntVector& Value)
{
    Writer.WriteObjectStart(Key);
    Writer.WriteValue(TEXT("x"), Value.X);
    Writer.WriteValue(TEXT("y"), Value.Y);
    Writer.WriteValue(TEXT("z"), Value.Z);
    Writer.WriteObjectEnd();
}

void WriteJsonBounds(
    FExploreJsonWriter& Writer,
    const TCHAR* Key,
    const FIntVector& Origin,
    int32 Size,
    float VoxelToMeters)
{
    Writer.WriteObjectStart(Key);
    Writer.WriteObjectStart(TEXT("min"));
    Writer.WriteValue(TEXT("x"), Origin.X);
    Writer.WriteValue(TEXT("y"), Origin.Y);
    Writer.WriteValue(TEXT("z"), Origin.Z);
    Writer.WriteObjectEnd();
    Writer.WriteObjectStart(TEXT("max_exclusive"));
    Writer.WriteValue(TEXT("x"), Origin.X + Size);
    Writer.WriteValue(TEXT("y"), Origin.Y + Size);
    Writer.WriteValue(TEXT("z"), Origin.Z + Size);
    Writer.WriteObjectEnd();
    Writer.WriteValue(TEXT("units"), TEXT("voxels"));
    Writer.WriteValue(TEXT("voxel_size_m"), static_cast<double>(VoxelToMeters));
    Writer.WriteObjectEnd();
}

FString BuildManifestJson(
    const FExploreArguments& Arguments,
    const FExploreExportOutput& Export)
{
    FString Json;
    TSharedRef<FExploreJsonWriter> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
    Writer->WriteObjectStart();
    Writer->WriteValue(TEXT("schema_version"), 2);
    Writer->WriteValue(TEXT("format"), TEXT("OBJ"));
    Writer->WriteValue(TEXT("mesh_file"), Export.MeshFileName);
    Writer->WriteValue(TEXT("mesh_positions_units"), TEXT("metres"));
    Writer->WriteValue(TEXT("mesh_uv_units"), TEXT("voxels"));
    Writer->WriteValue(TEXT("load_notes"), TEXT(
        "Load geometry.obj directly in a browser OBJ loader; do not regenerate density. "
        "The manifest bounds are max-exclusive voxel bounds, OBJ positions are metres, and OBJ UVs "
        "are the canonical voxel-space UVs."));
    Writer->WriteValue(TEXT("skirts"), false);
    Writer->WriteValue(TEXT("vertex_count"), Export.VertexCount);
    Writer->WriteValue(TEXT("triangle_count"), Export.TriangleCount);
    Writer->WriteValue(TEXT("mesh_file_size_bytes"), Export.MeshFileSizeBytes);
    Writer->WriteValue(TEXT("mesh_array_bytes_sum_over_tiles"), Export.MeshArrayBytes);
    Writer->WriteValue(TEXT("canonical_mesher_tile_cells"), CHUNK_SIZE);
    Writer->WriteValue(TEXT("canonical_mesher_tile_count"), Export.MesherTileCount);
    Writer->WriteValue(TEXT("seed"), Arguments.Seed);
    Writer->WriteValue(TEXT("archetype"), ArchetypeName(Arguments.Archetype));
    Writer->WriteValue(TEXT("slot"), Arguments.Slot);
    WriteJsonIntVector(*Writer, TEXT("region_origin_voxels"), Export.RegionOrigin);
    WriteJsonBounds(*Writer, TEXT("region_bounds_voxels"), Export.RegionOrigin, Export.RegionSize, 0.25f);
    Writer->WriteObjectStart(TEXT("region_bounds_metres"));
    Writer->WriteObjectStart(TEXT("min"));
    Writer->WriteValue(TEXT("x"), static_cast<double>(Export.RegionOrigin.X) * 0.25);
    Writer->WriteValue(TEXT("y"), static_cast<double>(Export.RegionOrigin.Y) * 0.25);
    Writer->WriteValue(TEXT("z"), static_cast<double>(Export.RegionOrigin.Z) * 0.25);
    Writer->WriteObjectEnd();
    Writer->WriteObjectStart(TEXT("max_exclusive"));
    Writer->WriteValue(TEXT("x"), static_cast<double>(Export.RegionOrigin.X + Export.RegionSize) * 0.25);
    Writer->WriteValue(TEXT("y"), static_cast<double>(Export.RegionOrigin.Y + Export.RegionSize) * 0.25);
    Writer->WriteValue(TEXT("z"), static_cast<double>(Export.RegionOrigin.Z + Export.RegionSize) * 0.25);
    Writer->WriteObjectEnd();
    Writer->WriteValue(TEXT("units"), TEXT("metres"));
    Writer->WriteObjectEnd();
    Writer->WriteObjectStart(TEXT("player_dimensions"));
    Writer->WriteValue(TEXT("capsule_radius_m"), static_cast<double>(FVoxelPlayerCapsuleConstants::RadiusCentimeters) / 100.0);
    Writer->WriteValue(TEXT("capsule_width_m"), static_cast<double>(2.0f * FVoxelPlayerCapsuleConstants::RadiusCentimeters) / 100.0);
    Writer->WriteValue(TEXT("capsule_half_height_m"), static_cast<double>(FVoxelPlayerCapsuleConstants::HalfHeightCentimeters) / 100.0);
    Writer->WriteValue(TEXT("capsule_height_m"), static_cast<double>(2.0f * FVoxelPlayerCapsuleConstants::HalfHeightCentimeters) / 100.0);
    Writer->WriteValue(TEXT("max_step_height_m"), static_cast<double>(FVoxelPlayerCapsuleConstants::MaxStepHeightMeters));
    Writer->WriteObjectEnd();
    Writer->WriteObjectStart(TEXT("world_invariants"));
    Writer->WriteValue(TEXT("world_radius_voxels"), 0);
    Writer->WriteValue(TEXT("lateral_regions_enabled"), false);
    Writer->WriteValue(TEXT("diff_layer_mutated"), false);
    Writer->WriteObjectEnd();
    Writer->WriteObjectEnd();
    return Writer->Close() ? Json : FString();
}

bool RunExport(
    const FExploreArguments& Arguments,
    FExploreWorld& World,
    FExploreExportOutput& OutOutput)
{
    OutOutput = FExploreExportOutput();
    OutOutput.RegionSize = Arguments.ExportSize;
    const int32 TilesPerAxis = Arguments.ExportSize / CHUNK_SIZE;
    OutOutput.MesherTileCount = TilesPerAxis * TilesPerAxis * TilesPerAxis;
    const int32 TargetMiddleZ = FMath::FloorToInt(
        0.5f * static_cast<float>(World.TargetBottomWorldZ + World.TargetTopWorldZ));
    OutOutput.RegionOrigin = FIntVector(
        -Arguments.ExportSize / 2,
        -Arguments.ExportSize / 2,
        TargetMiddleZ - Arguments.ExportSize / 2);
    OutOutput.EstimatedWorkingBytes = EstimateExportWorkingBytes(Arguments.ExportSize);
    if (OutOutput.EstimatedWorkingBytes > OutOutput.WorkingMemoryCapBytes)
    {
        OutOutput.Status = TEXT("refused");
        OutOutput.RefusalReason = FString::Printf(
            TEXT("Refused before meshing: estimated working memory %lld bytes exceeds cap %lld bytes."),
            static_cast<long long>(OutOutput.EstimatedWorkingBytes),
            static_cast<long long>(OutOutput.WorkingMemoryCapBytes));
        return false;
    }

    // Skirts are a tile-rendering seam aid and extend beyond a bounded export box. The mesh is
    // still generated by the canonical mesher; disabling only that presentation option keeps the
    // browser asset's geometry inside the manifest bounds.
    World.Mesher->bGenerateSkirts = false;
    FString Error;
    OutOutput.MeshFileName = TEXT("geometry.obj");
    OutOutput.ManifestFileName = TEXT("manifest.json");
    const FString MeshPath = FPaths::Combine(Arguments.OutDirectory, OutOutput.MeshFileName);
    TUniquePtr<FArchive> Archive(IFileManager::Get().CreateFileWriter(*MeshPath));
    if (!Archive.IsValid())
    {
        OutOutput.Status = TEXT("error");
        OutOutput.RefusalReason = FString::Printf(TEXT("Could not create OBJ '%s'."), *MeshPath);
        return false;
    }
    if (!WriteObjHeader(*Archive))
    {
        OutOutput.Status = TEXT("error");
        OutOutput.RefusalReason = TEXT("Could not write the OBJ header.");
        return false;
    }

    int32 VertexOffset = 0;
    for (int32 TileZ = 0; TileZ < TilesPerAxis; ++TileZ)
    {
        for (int32 TileY = 0; TileY < TilesPerAxis; ++TileY)
        {
            for (int32 TileX = 0; TileX < TilesPerAxis; ++TileX)
            {
                const FIntVector TileOrigin = OutOutput.RegionOrigin
                    + FIntVector(TileX * CHUNK_SIZE, TileY * CHUNK_SIZE, TileZ * CHUNK_SIZE);
                const FVoxelMeshData TileMesh = World.Mesher->GenerateMesh(
                    TileOrigin,
                    1,
                    CHUNK_SIZE);
                if (!WriteObjTile(*Archive, TileMesh, VertexOffset, Error))
                {
                    OutOutput.Status = TEXT("error");
                    OutOutput.RefusalReason = Error;
                    return false;
                }
                const int64 TileArrayBytes = static_cast<int64>(TileMesh.Vertices.Num()) * sizeof(FVector)
                    + static_cast<int64>(TileMesh.Normals.Num()) * sizeof(FVector)
                    + static_cast<int64>(TileMesh.UVs.Num()) * sizeof(FVector2D)
                    + static_cast<int64>(TileMesh.Colors.Num()) * sizeof(FColor)
                    + static_cast<int64>(TileMesh.Triangles.Num()) * sizeof(int32);
                OutOutput.MeshArrayBytes += TileArrayBytes;
                OutOutput.VertexCount += TileMesh.Vertices.Num();
                OutOutput.TriangleCount += TileMesh.Triangles.Num() / 3;
                VertexOffset += TileMesh.Vertices.Num();
            }
        }
    }
    if (Archive->IsError())
    {
        OutOutput.Status = TEXT("error");
        OutOutput.RefusalReason = TEXT("Could not finish the streamed OBJ.");
        return false;
    }
    Archive.Reset();
    OutOutput.MeshFileSizeBytes = IFileManager::Get().FileSize(*MeshPath);
    if (OutOutput.MeshFileSizeBytes < 0)
    {
        OutOutput.Status = TEXT("error");
        OutOutput.RefusalReason = TEXT("OBJ was written but its file size could not be read.");
        return false;
    }

    const FString ManifestJson = BuildManifestJson(Arguments, OutOutput);
    const FString ManifestJsonRepeat = BuildManifestJson(Arguments, OutOutput);
    if (ManifestJson.IsEmpty() || ManifestJsonRepeat.IsEmpty())
    {
        OutOutput.Status = TEXT("error");
        OutOutput.RefusalReason = TEXT("The export manifest JSON writer failed.");
        return false;
    }
    if (ManifestJson != ManifestJsonRepeat)
    {
        OutOutput.Status = TEXT("error");
        OutOutput.RefusalReason = TEXT("The export manifest failed its deterministic repeat assertion.");
        return false;
    }
    if (!FFileHelper::SaveStringToFile(
            ManifestJson,
            *FPaths::Combine(Arguments.OutDirectory, OutOutput.ManifestFileName),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        OutOutput.Status = TEXT("error");
        OutOutput.RefusalReason = TEXT("Could not write export manifest.json.");
        return false;
    }
    OutOutput.Status = TEXT("ok");
    return true;
}

void WritePlayerDimensions(FExploreJsonWriter& Writer, const TCHAR* Key)
{
    Writer.WriteObjectStart(Key);
    Writer.WriteValue(TEXT("capsule_radius_m"), static_cast<double>(FVoxelPlayerCapsuleConstants::RadiusCentimeters) / 100.0);
    Writer.WriteValue(TEXT("capsule_width_m"), static_cast<double>(2.0f * FVoxelPlayerCapsuleConstants::RadiusCentimeters) / 100.0);
    Writer.WriteValue(TEXT("capsule_half_height_m"), static_cast<double>(FVoxelPlayerCapsuleConstants::HalfHeightCentimeters) / 100.0);
    Writer.WriteValue(TEXT("capsule_height_m"), static_cast<double>(2.0f * FVoxelPlayerCapsuleConstants::HalfHeightCentimeters) / 100.0);
    Writer.WriteValue(TEXT("max_step_height_m"), static_cast<double>(FVoxelPlayerCapsuleConstants::MaxStepHeightMeters));
    Writer.WriteValue(TEXT("walkable_floor_angle_degrees"), static_cast<double>(FVoxelPlayerCapsuleConstants::WalkableFloorAngleDegrees));
    Writer.WriteObjectEnd();
}

FString BuildExploreJson(
    const FExploreArguments& Arguments,
    const FExploreWorld& World,
    const FExploreRunOutput& Output,
    bool bDeterminismAssertion,
    uint32 PayloadCrc32)
{
    FString Json;
    TSharedRef<FExploreJsonWriter> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
    Writer->WriteObjectStart();
    Writer->WriteValue(TEXT("schema_version"), 2);
    Writer->WriteValue(TEXT("tool"), TEXT("VoxelForgeExplore"));
    Writer->WriteValue(TEXT("read_only_generation"), true);

    Writer->WriteObjectStart(TEXT("input"));
    Writer->WriteValue(TEXT("seed"), Arguments.Seed);
    Writer->WriteValue(TEXT("archetype"), ArchetypeName(Arguments.Archetype));
    Writer->WriteValue(TEXT("slot"), Arguments.Slot);
    Writer->WriteArrayStart(TEXT("modes"));
    if (Arguments.bRender) Writer->WriteValue(TEXT("render"));
    if (Arguments.bWalk) Writer->WriteValue(TEXT("walk"));
    if (Arguments.bExport) Writer->WriteValue(TEXT("export"));
    Writer->WriteArrayEnd();
    Writer->WriteValue(TEXT("failure_focus_render"), Arguments.bFailureFocusRender);
    Writer->WriteValue(TEXT("out_directory"), Arguments.OutDirectory);
    Writer->WriteValue(TEXT("canonical_invocation"), FString::Printf(
        TEXT("UnrealEditor-Cmd VoxelM.uproject -run=VoxelForgeExplore -seed=%d -archetype=%s "
             "-slot=%d -modes=%s -out=Saved/VoxelForge/Explore/<runid>"),
        Arguments.Seed,
        ArchetypeName(Arguments.Archetype),
        Arguments.Slot,
        *Arguments.CanonicalModes()));
    Writer->WriteObjectEnd();

    Writer->WriteObjectStart(TEXT("world"));
    Writer->WriteValue(TEXT("voxel_size_m"), 0.25);
    Writer->WriteValue(TEXT("world_radius_voxels"), 0);
    Writer->WriteValue(TEXT("lateral_regions_enabled"), false);
    Writer->WriteValue(TEXT("diff_layer_mutated"), false);
    Writer->WriteValue(TEXT("target_strate_slot"), Arguments.Slot);
    Writer->WriteValue(TEXT("target_bottom_voxel_z"), World.TargetBottomWorldZ);
    Writer->WriteValue(TEXT("target_top_voxel_z_exclusive"), World.TargetTopWorldZ);
    Writer->WriteValue(TEXT("target_bottom_m"), static_cast<double>(World.TargetBottomWorldZ) * 0.25);
    Writer->WriteValue(TEXT("target_top_m_exclusive"), static_cast<double>(World.TargetTopWorldZ) * 0.25);
    Writer->WriteValue(TEXT("target_boundary_seal_thickness_voxels"),
        static_cast<double>(World.TargetBoundarySealThickness));
    WritePlayerDimensions(*Writer, TEXT("player_dimensions"));
    Writer->WriteObjectEnd();

    if (Arguments.bRender)
    {
        Writer->WriteObjectStart(TEXT("render"));
        Writer->WriteValue(TEXT("status"), Output.Render.Status);
        Writer->WriteValue(TEXT("refusal_or_error"), Output.Render.RefusalReason);
        Writer->WriteValue(TEXT("width_px"), Output.Render.Width);
        Writer->WriteValue(TEXT("height_px"), Output.Render.Height);
        Writer->WriteValue(TEXT("max_pixels_cap"), MaxRenderPixels);
        Writer->WriteValue(TEXT("max_fixed_density_samples_cap"), MaxRenderDensitySamples);
        Writer->WriteValue(TEXT("estimated_fixed_density_samples"),
            Output.Render.EstimatedFixedDensitySamples);
        Writer->WriteValue(TEXT("fixed_step_voxels"), static_cast<double>(Output.Render.FixedStepVoxels));
        Writer->WriteValue(TEXT("fixed_step_m"), static_cast<double>(Output.Render.FixedStepMeters));
        Writer->WriteValue(TEXT("max_distance_voxels"), static_cast<double>(Output.Render.MaxDistanceVoxels));
        Writer->WriteValue(TEXT("bisection_iterations"), Output.Render.BisectionIterations);
        Writer->WriteValue(TEXT("human_marker_height_m"), 1.76);
        Writer->WriteValue(TEXT("step_rationale"), Output.Render.StepRationale);
        Writer->WriteValue(TEXT("camera_seed_policy"), Output.Render.CameraSeedPolicy);
        Writer->WriteValue(TEXT("camera_seed_count"), Output.Render.CameraSeedCount);
        Writer->WriteValue(TEXT("all_cameras_player_fit"), Output.Render.bAllCamerasPlayerFit);
        Writer->WriteArrayStart(TEXT("images"));
        for (const FExploreRenderFrame& Frame : Output.Render.Frames)
        {
            Writer->WriteObjectStart();
            Writer->WriteValue(TEXT("purpose"), Frame.Purpose);
            Writer->WriteValue(TEXT("camera_seed_source"), Frame.CameraSeedSource);
            Writer->WriteValue(TEXT("file"), Frame.FileName);
            WriteJsonVector(*Writer, TEXT("camera_voxels"), Frame.CameraVoxels, 1.0f);
            WriteJsonVector(*Writer, TEXT("target_voxels"), Frame.TargetVoxels, 1.0f);
            Writer->WriteValue(TEXT("hit_pixels"), Frame.HitPixels);
            Writer->WriteValue(TEXT("scale_marker_projected"), Frame.bScaleMarkerProjected);
            Writer->WriteValue(TEXT("failure_markers_projected"), Frame.bFailureMarkersProjected);
            Writer->WriteValue(TEXT("scale_marker_height_m"), 1.76);
            Writer->WriteObjectEnd();
        }
        Writer->WriteArrayEnd();
        Writer->WriteObjectEnd();
    }

    if (Arguments.bWalk)
    {
        const FVoxelPlayerFitWalkReport& Report = Output.Walk.Report;
        // The primary report is the mouth-sized arrival/departure window.  Origin reachability is
        // a separate, explicitly origin-inclusive measurement; expose that report at the legacy
        // top-level keys so a sweep cannot accidentally count a window that omitted (0,0).
        const FVoxelPlayerFitWalkReport& OriginReport = Output.Walk.bOriginCheckAvailable
            ? Output.Walk.OriginCheckReport : Report;
        Writer->WriteObjectStart(TEXT("walk"));
        Writer->WriteValue(TEXT("status"), Output.Walk.Status);
        Writer->WriteValue(TEXT("refusal_or_error"), Output.Walk.RefusalReason);
        Writer->WriteValue(TEXT("window_policy"), Output.Walk.WindowPolicy);
        Writer->WriteValue(TEXT("agent_policy"), TEXT(
            "fixed-order depth-first walk over the exact VF player-fit six-neighbour graph; stop at departure"));
        Writer->WriteValue(TEXT("player_fit_volume_scope"), TEXT(
            "bounded fitted route window; WorldRadiusVoxels=0 has no finite whole-world volume"));
        Writer->WriteValue(TEXT("max_cells_cap"), Arguments.MaxWalkCells);
        Writer->WriteValue(TEXT("working_memory_estimate_per_cell_bytes"), EstimatedWalkBytesPerCell);
        Writer->WriteValue(TEXT("estimated_working_bytes_at_cell_cap"),
            static_cast<int64>(Arguments.MaxWalkCells) * EstimatedWalkBytesPerCell);
        Writer->WriteValue(TEXT("working_memory_cap_bytes"), MaxWalkWorkingBytes);
        Writer->WriteValue(TEXT("arrival_passage_count"), Output.Walk.ArrivalPassageCount);
        Writer->WriteValue(TEXT("departure_passage_count"), Output.Walk.DeparturePassageCount);
        Writer->WriteValue(TEXT("landing_room_probe_offsets_voxels"), 2.0);
        Writer->WriteValue(TEXT("arrival_landing_room_probe_count"),
            Output.Walk.ArrivalLandingRoomProbeCount);
        Writer->WriteValue(TEXT("arrival_landing_room_player_fit_probe_count"),
            Output.Walk.ArrivalLandingRoomPlayerFitProbeCount);
        Writer->WriteValue(TEXT("arrival_landing_room_arrival_component_probe_count"),
            Output.Walk.ArrivalLandingRoomArrivalComponentProbeCount);
        Writer->WriteValue(TEXT("arrival_landing_room_in_arrival_component"),
            Output.Walk.ArrivalLandingRoomProbeCount > 0
            && Output.Walk.ArrivalLandingRoomArrivalComponentProbeCount
                == Output.Walk.ArrivalLandingRoomProbeCount);
        Writer->WriteValue(TEXT("departure_landing_room_probe_count"),
            Output.Walk.DepartureLandingRoomProbeCount);
        Writer->WriteValue(TEXT("departure_landing_room_player_fit_probe_count"),
            Output.Walk.DepartureLandingRoomPlayerFitProbeCount);
        Writer->WriteValue(TEXT("departure_landing_room_arrival_component_probe_count"),
            Output.Walk.DepartureLandingRoomArrivalComponentProbeCount);
        Writer->WriteValue(TEXT("departure_landing_room_in_arrival_component"),
            Output.Walk.DepartureLandingRoomProbeCount > 0
            && Output.Walk.DepartureLandingRoomArrivalComponentProbeCount
                == Output.Walk.DepartureLandingRoomProbeCount);
        if (Output.Walk.bHasArrival)
        {
            WriteJsonVector(*Writer, TEXT("arrival_mouth_voxels"), Output.Walk.ArrivalVoxels, 1.0f);
            WriteJsonVector(*Writer, TEXT("arrival_mouth_metres"), Output.Walk.ArrivalVoxels, 0.25f);
        }
        if (Output.Walk.bHasDeparture)
        {
            WriteJsonVector(*Writer, TEXT("departure_mouth_voxels"), Output.Walk.DepartureVoxels, 1.0f);
            WriteJsonVector(*Writer, TEXT("departure_mouth_metres"), Output.Walk.DepartureVoxels, 0.25f);
        }
        Writer->WriteValue(TEXT("connectivity_result"), ConnectivityResultName(Report.Result));
        Writer->WriteValue(TEXT("connectivity_result_code"), static_cast<int32>(Report.Result));
        Writer->WriteValue(TEXT("can_reach_departure"), Report.bCanReachDeparture);
        Writer->WriteValue(TEXT("start_snapped"), Report.bStartSnapped);
        Writer->WriteValue(TEXT("goal_snapped"), Report.bGoalSnapped);
        Writer->WriteValue(TEXT("route_retries"), Report.NumRouteRetries);
        Writer->WriteValue(TEXT("player_fit_volume_cells"), Report.PlayerFitVolumeCells);
        Writer->WriteValue(TEXT("reachable_player_fit_cells"), Report.ReachablePlayerFitCells);
        Writer->WriteValue(TEXT("reachable_player_fit_fraction"), static_cast<double>(Report.ReachablePlayerFitFraction));
        Writer->WriteValue(TEXT("sampled_num_x"), Report.SampledNumX);
        Writer->WriteValue(TEXT("sampled_num_y"), Report.SampledNumY);
        Writer->WriteValue(TEXT("sampled_num_z"), Report.SampledNumZ);
        Writer->WriteValue(TEXT("sampled_bounds_units"), TEXT("voxels"));
        Writer->WriteValue(TEXT("sampled_min_z"), Report.SampledMinZ);
        Writer->WriteValue(TEXT("sampled_max_z_exclusive"), Report.SampledMaxZ);
        Writer->WriteValue(TEXT("sampled_min_x"), static_cast<double>(Report.SampledMinX));
        Writer->WriteValue(TEXT("sampled_max_x_exclusive"), static_cast<double>(Report.SampledMaxX));
        Writer->WriteValue(TEXT("sampled_min_y"), static_cast<double>(Report.SampledMinY));
        Writer->WriteValue(TEXT("sampled_max_y_exclusive"), static_cast<double>(Report.SampledMaxY));
        Writer->WriteValue(TEXT("agent_graph_traversals"), Report.AgentGraphTraversals);
        Writer->WriteValue(TEXT("distance_travelled_m"), static_cast<double>(Report.DistanceTravelledMeters));
        Writer->WriteValue(TEXT("straight_line_m"), static_cast<double>(Report.StraightLineMeters));
        Writer->WriteValue(TEXT("tortuosity"), static_cast<double>(Report.Tortuosity));
        Writer->WriteValue(TEXT("dead_ends_encountered"), Report.DeadEndsEncountered);
        Writer->WriteValue(TEXT("dead_ends_per_100m"), static_cast<double>(Report.DeadEndsPer100m));
        Writer->WriteValue(TEXT("capsule_radius_m"), static_cast<double>(Report.CapsuleRadiusMeters));
        Writer->WriteValue(TEXT("capsule_width_m"), static_cast<double>(Report.CapsuleWidthMeters));
        Writer->WriteValue(TEXT("capsule_height_m"), static_cast<double>(Report.CapsuleHeightMeters));
        Writer->WriteValue(TEXT("narrow_gap_threshold_m"), static_cast<double>(Report.NarrowGapThresholdMeters));
        Writer->WriteValue(TEXT("narrow_gap_traversals"), Report.NarrowGapTraversals);
        Writer->WriteValue(TEXT("narrow_gap_fraction"), static_cast<double>(Report.NarrowGapFraction));
        Writer->WriteValue(TEXT("narrow_gap_events_per_100m"), static_cast<double>(Report.NarrowGapEventsPer100m));
        Writer->WriteValue(TEXT("narrow_gap_definition"), Report.NarrowGapDefinition);
        Writer->WriteValue(TEXT("player_fit_components"), Report.PlayerFitComponents);
        Writer->WriteValue(TEXT("largest_player_fit_component_cells"), Report.LargestPlayerFitComponentCells);
        Writer->WriteValue(TEXT("arrival_component_cells"), Report.ArrivalComponentCells);
        Writer->WriteValue(TEXT("departure_component_cells"), Report.DepartureComponentCells);
        Writer->WriteValue(TEXT("mouth_component_gap_voxels"), static_cast<double>(Report.MouthComponentGapVoxels));
        Writer->WriteValue(TEXT("mouth_component_gap_m"), static_cast<double>(Report.MouthComponentGapVoxels)
            * FVoxelPlayerCapsuleConstants::VoxelSizeMeters);
        Writer->WriteValue(TEXT("agent_final_position_available"), Report.bHasAgentFinalPosition);
        if (Report.bHasAgentFinalPosition)
        {
            WriteJsonVector(*Writer, TEXT("agent_final_position_voxels"), Report.AgentFinalVoxels, 1.0f);
            WriteJsonVector(*Writer, TEXT("agent_final_position_metres"), Report.AgentFinalVoxels, 0.25f);
        }
        Writer->WriteValue(TEXT("last_reached_position_available"), Report.bHasLastReachedPosition);
        if (Report.bHasLastReachedPosition)
        {
            WriteJsonVector(*Writer, TEXT("last_reached_position_voxels"), Report.LastReachedVoxels, 1.0f);
            WriteJsonVector(*Writer, TEXT("last_reached_position_metres"), Report.LastReachedVoxels, 0.25f);
        }
        Writer->WriteValue(TEXT("target_component_nearest_cell_available"), Report.bHasTargetComponentNearestCell);
        if (Report.bHasTargetComponentNearestCell)
        {
            WriteJsonVector(*Writer, TEXT("target_component_nearest_cell_voxels"),
                Report.TargetComponentNearestVoxels, 1.0f);
            WriteJsonVector(*Writer, TEXT("target_component_nearest_cell_metres"),
                Report.TargetComponentNearestVoxels, 0.25f);
        }
        Writer->WriteValue(TEXT("agent_to_target_component_gap_voxels"),
            static_cast<double>(Report.AgentToTargetComponentGapVoxels));
        Writer->WriteValue(TEXT("agent_to_target_component_gap_m"),
            static_cast<double>(Report.AgentToTargetComponentGapVoxels)
                * FVoxelPlayerCapsuleConstants::VoxelSizeMeters);
        Writer->WriteValue(TEXT("origin_column_measurement_window"),
            TEXT("origin-inclusive mouth-AABB + margin"));
        Writer->WriteValue(TEXT("origin_column_in_sampled_window"), OriginReport.bOriginColumnInSampledWindow);
        Writer->WriteValue(TEXT("origin_column_has_player_fit"), OriginReport.bOriginColumnHasPlayerFit);
        Writer->WriteValue(TEXT("origin_column_player_fit_cells"), OriginReport.OriginColumnPlayerFitCells);
        Writer->WriteValue(TEXT("origin_column_reachable_from_arrival"), OriginReport.bOriginColumnReachable);
        Writer->WriteValue(TEXT("reachable_set_to_origin_column_voxels"),
            static_cast<double>(OriginReport.ReachableSetToOriginColumnVoxels));
        Writer->WriteValue(TEXT("reachable_set_to_origin_column_m"),
            static_cast<double>(OriginReport.ReachableSetToOriginColumnVoxels)
                * FVoxelPlayerCapsuleConstants::VoxelSizeMeters);
        Writer->WriteObjectStart(TEXT("origin_check"));
        Writer->WriteValue(TEXT("attempted"), Output.Walk.bOriginCheckAttempted);
        Writer->WriteValue(TEXT("available"), Output.Walk.bOriginCheckAvailable);
        Writer->WriteValue(TEXT("refusal_or_error"), Output.Walk.OriginCheckRefusalReason);
        if (Output.Walk.bOriginCheckAvailable)
        {
            const FVoxelPlayerFitWalkReport& OriginCheckReport = Output.Walk.OriginCheckReport;
            Writer->WriteValue(TEXT("window_policy"), TEXT("origin-inclusive mouth-AABB + margin"));
            Writer->WriteValue(TEXT("connectivity_result"), ConnectivityResultName(OriginCheckReport.Result));
            Writer->WriteValue(TEXT("can_reach_departure"), OriginCheckReport.bCanReachDeparture);
            Writer->WriteValue(TEXT("sampled_num_x"), OriginCheckReport.SampledNumX);
            Writer->WriteValue(TEXT("sampled_num_y"), OriginCheckReport.SampledNumY);
            Writer->WriteValue(TEXT("sampled_num_z"), OriginCheckReport.SampledNumZ);
            Writer->WriteValue(TEXT("sampled_min_x"), static_cast<double>(OriginCheckReport.SampledMinX));
            Writer->WriteValue(TEXT("sampled_max_x_exclusive"), static_cast<double>(OriginCheckReport.SampledMaxX));
            Writer->WriteValue(TEXT("sampled_min_y"), static_cast<double>(OriginCheckReport.SampledMinY));
            Writer->WriteValue(TEXT("sampled_max_y_exclusive"), static_cast<double>(OriginCheckReport.SampledMaxY));
            Writer->WriteValue(TEXT("sampled_min_z"), OriginCheckReport.SampledMinZ);
            Writer->WriteValue(TEXT("sampled_max_z_exclusive"), OriginCheckReport.SampledMaxZ);
            Writer->WriteValue(TEXT("player_fit_cells"), OriginCheckReport.PlayerFitVolumeCells);
            Writer->WriteValue(TEXT("reachable_player_fit_cells"), OriginCheckReport.ReachablePlayerFitCells);
            Writer->WriteValue(TEXT("reachable_player_fit_fraction"),
                static_cast<double>(OriginCheckReport.ReachablePlayerFitFraction));
            Writer->WriteValue(TEXT("player_fit_components"), OriginCheckReport.PlayerFitComponents);
            Writer->WriteValue(TEXT("arrival_component_cells"), OriginCheckReport.ArrivalComponentCells);
            Writer->WriteValue(TEXT("departure_component_cells"), OriginCheckReport.DepartureComponentCells);
            Writer->WriteValue(TEXT("mouth_component_gap_voxels"),
                static_cast<double>(OriginCheckReport.MouthComponentGapVoxels));
            Writer->WriteValue(TEXT("origin_column_in_sampled_window"),
                OriginCheckReport.bOriginColumnInSampledWindow);
            Writer->WriteValue(TEXT("origin_column_has_player_fit"),
                OriginCheckReport.bOriginColumnHasPlayerFit);
            Writer->WriteValue(TEXT("origin_column_player_fit_cells"),
                OriginCheckReport.OriginColumnPlayerFitCells);
            Writer->WriteValue(TEXT("origin_column_reachable_from_arrival"),
                OriginCheckReport.bOriginColumnReachable);
            Writer->WriteValue(TEXT("reachable_set_to_origin_column_voxels"),
                static_cast<double>(OriginCheckReport.ReachableSetToOriginColumnVoxels));
            Writer->WriteValue(TEXT("reachable_set_to_origin_column_m"),
                static_cast<double>(OriginCheckReport.ReachableSetToOriginColumnVoxels)
                    * FVoxelPlayerCapsuleConstants::VoxelSizeMeters);
        }
        Writer->WriteObjectEnd();
        Writer->WriteObjectEnd();
    }

    if (Arguments.bExport)
    {
        Writer->WriteObjectStart(TEXT("export"));
        Writer->WriteValue(TEXT("status"), Output.Export.Status);
        Writer->WriteValue(TEXT("refusal_or_error"), Output.Export.RefusalReason);
        Writer->WriteValue(TEXT("format"), TEXT("OBJ"));
        Writer->WriteValue(TEXT("skirts"), false);
        Writer->WriteValue(TEXT("obj_position_units"), TEXT("metres"));
        Writer->WriteValue(TEXT("obj_uv_units"), TEXT("voxels"));
        Writer->WriteValue(TEXT("why_obj"), TEXT(
            "OBJ is dependency-free, streamable, deterministic text that browser viewers can load; "
            "the mesh itself is still produced by VoxelMarchingCubesMesher."));
        Writer->WriteValue(TEXT("mesh_file"), Output.Export.MeshFileName);
        Writer->WriteValue(TEXT("manifest_file"), Output.Export.ManifestFileName);
        WriteJsonIntVector(*Writer, TEXT("region_origin_voxels"), Output.Export.RegionOrigin);
        WriteJsonBounds(*Writer, TEXT("region_bounds_voxels"), Output.Export.RegionOrigin, Output.Export.RegionSize, 0.25f);
        Writer->WriteValue(TEXT("region_size_cells_per_axis"), Output.Export.RegionSize);
        Writer->WriteValue(TEXT("canonical_mesher_tile_cells"), CHUNK_SIZE);
        Writer->WriteValue(TEXT("canonical_mesher_tile_count"), Output.Export.MesherTileCount);
        Writer->WriteValue(TEXT("estimated_working_bytes"), Output.Export.EstimatedWorkingBytes);
        Writer->WriteValue(TEXT("working_memory_cap_bytes"), Output.Export.WorkingMemoryCapBytes);
        Writer->WriteValue(TEXT("mesh_array_bytes"), Output.Export.MeshArrayBytes);
        Writer->WriteValue(TEXT("mesh_array_bytes_sum_over_tiles"), Output.Export.MeshArrayBytes);
        Writer->WriteValue(TEXT("mesh_file_size_bytes"), Output.Export.MeshFileSizeBytes);
        Writer->WriteValue(TEXT("vertex_count"), Output.Export.VertexCount);
        Writer->WriteValue(TEXT("triangle_count"), Output.Export.TriangleCount);
        Writer->WriteObjectEnd();
    }

    Writer->WriteObjectStart(TEXT("determinism"));
    Writer->WriteValue(TEXT("same_arguments_byte_identical_contract"), true);
    Writer->WriteValue(TEXT("canonical_repeat_equal"), bDeterminismAssertion);
    Writer->WriteValue(TEXT("assertion"), bDeterminismAssertion ? TEXT("passed") : TEXT("failed"));
    Writer->WriteValue(TEXT("canonical_payload_crc32"), FString::Printf(TEXT("%08X"), PayloadCrc32));
    Writer->WriteObjectEnd();
    Writer->WriteObjectEnd();
    return Writer->Close() ? Json : FString();
}

} // namespace

UVoxelForgeExploreCommandlet::UVoxelForgeExploreCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
}

int32 UVoxelForgeExploreCommandlet::Main(const FString& Params)
{
    const double MainStartSeconds = FPlatformTime::Seconds();
    UE_LOG(LogTemp, Display, TEXT("[VoxelForgeExplore] invoked: %s"), FCommandLine::Get());

    FExploreArguments Arguments;
    FString Error;
    if (!ParseArguments(Params, Arguments, Error))
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] %s"), *Error);
        return 1;
    }
    if (!IFileManager::Get().MakeDirectory(*Arguments.OutDirectory, true))
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] Could not create output directory '%s'."),
            *Arguments.OutDirectory);
        return 1;
    }

    const double SetupStartSeconds = FPlatformTime::Seconds();
    FExploreWorld World;
    if (!World.Build(Arguments, Error))
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] %s"), *Error);
        return 1;
    }
    const double SetupSeconds = FPlatformTime::Seconds() - SetupStartSeconds;

    FExploreRunOutput Output;
    bool bRequestedModeFailed = false;
    FExploreWalkOutput RenderSeedWalk;
    const FExploreWalkOutput* CameraSeedWalk = nullptr;
    if (Arguments.bWalk)
    {
        const double Start = FPlatformTime::Seconds();
        if (!RunWalk(Arguments, World, Output.Walk))
        {
            bRequestedModeFailed = true;
        }
        UE_LOG(LogTemp, Display, TEXT("[VoxelForgeExplore] walk %.3fs (%s)"),
            FPlatformTime::Seconds() - Start, *Output.Walk.Status);
        CameraSeedWalk = &Output.Walk;
    }
    else if (Arguments.bRender)
    {
        // A render-only invocation still has to obey the inside-walkable-space contract. Run the
        // same focused walk as a private seed pass; it is intentionally omitted from the render-only
        // JSON so the selected mode remains render.
        const double Start = FPlatformTime::Seconds();
        if (!RunWalk(Arguments, World, RenderSeedWalk))
        {
            bRequestedModeFailed = true;
        }
        UE_LOG(LogTemp, Display, TEXT("[VoxelForgeExplore] render camera seed walk %.3fs (%s)"),
            FPlatformTime::Seconds() - Start, *RenderSeedWalk.Status);
        CameraSeedWalk = &RenderSeedWalk;
    }
    if (Arguments.bRender)
    {
        const double Start = FPlatformTime::Seconds();
        if (CameraSeedWalk == nullptr || !RunRender(Arguments, World, *CameraSeedWalk, Output.Render))
        {
            bRequestedModeFailed = true;
        }
        UE_LOG(LogTemp, Display, TEXT("[VoxelForgeExplore] render %.3fs (%s)"),
            FPlatformTime::Seconds() - Start, *Output.Render.Status);
    }
    if (Arguments.bFailureFocusRender && Arguments.bRender && Arguments.bWalk)
    {
        const double Start = FPlatformTime::Seconds();
        if (!RunFailureBoundaryRender(Arguments, World, Output.Walk, Output.Render))
        {
            bRequestedModeFailed = true;
            UE_LOG(LogTemp, Error,
                TEXT("[VoxelForgeExplore] failure-focus render could not produce a boundary image."));
        }
        else
        {
            UE_LOG(LogTemp, Display, TEXT("[VoxelForgeExplore] failure-focus render %.3fs (ok)"),
                FPlatformTime::Seconds() - Start);
        }
    }
    if (Arguments.bExport)
    {
        const double Start = FPlatformTime::Seconds();
        if (!RunExport(Arguments, World, Output.Export))
        {
            bRequestedModeFailed = true;
        }
        UE_LOG(LogTemp, Display, TEXT("[VoxelForgeExplore] export %.3fs (%s)"),
            FPlatformTime::Seconds() - Start, *Output.Export.Status);
    }

    // Build the payload twice before adding the assertion itself. This is deliberately a string
    // comparison over a fixed-order writer, not a TMap/pretty-printer whose key order could drift.
    const FString Payload = BuildExploreJson(Arguments, World, Output, false, 0);
    const FString PayloadRepeat = BuildExploreJson(Arguments, World, Output, false, 0);
    if (Payload.IsEmpty() || PayloadRepeat.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] JSON writer failed before report creation."));
        return 1;
    }
    // Hash the exact UTF-8 representation that is written to disk. This keeps the reported
    // digest independent of TCHAR width (and therefore meaningful when the same run is compared
    // across editor platforms).
    FTCHARToUTF8 PayloadUtf8(*Payload);
    const uint32 PayloadCrc32 = FCrc::MemCrc32(PayloadUtf8.Get(), PayloadUtf8.Length());
    const bool bPayloadRepeatEqual = Payload == PayloadRepeat;
    FString Json = BuildExploreJson(
        Arguments, World, Output, bPayloadRepeatEqual, PayloadCrc32);
    const FString JsonRepeat = BuildExploreJson(
        Arguments, World, Output, bPayloadRepeatEqual, PayloadCrc32);
    if (Json.IsEmpty() || JsonRepeat.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] JSON writer failed during report creation."));
        return 1;
    }
    const bool bFinalRepeatEqual = Json == JsonRepeat;
    if (!bFinalRepeatEqual)
    {
        UE_LOG(LogTemp, Error,
            TEXT("[VoxelForgeExplore] deterministic JSON assertion failed before write."));
        bRequestedModeFailed = true;
        Json = BuildExploreJson(Arguments, World, Output, false, PayloadCrc32);
    }

    const FString ReportPath = FPaths::Combine(Arguments.OutDirectory, TEXT("explore.json"));
    if (!FFileHelper::SaveStringToFile(
            Json,
            *ReportPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] Could not write '%s'."), *ReportPath);
        return 1;
    }

    UE_LOG(LogTemp, Display,
        TEXT("[VoxelForgeExplore] setup %.3fs, total %.3fs; report=%s; json_deterministic=%s"),
        SetupSeconds,
        FPlatformTime::Seconds() - MainStartSeconds,
        *ReportPath,
        (bPayloadRepeatEqual && bFinalRepeatEqual) ? TEXT("yes") : TEXT("no"));
    return bRequestedModeFailed ? 2 : 0;
}
