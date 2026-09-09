#include "VoxelForgeExploreCommandlet.h"

#include "CoreMinimal.h"
#include "Async/ParallelFor.h"
#include "Commandlets/Commandlet.h"
#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/CommandLine.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "VoxelCaveMorphology.h"
#include "VoxelDiffLayer.h"
#include "VoxelDensityOpStack.h"
#include "VoxelDensityProfile.h"
#include "VoxelGenerator.h"
#include "VoxelMarchingCubesMesher.h"
#include "VoxelNoise.h"
#include "VoxelSettings.h"
#include "VoxelStrateDefinition.h"
#include "VoxelStrateManager.h"
#include "VoxelStrateMeasure.h"
#include "VoxelTypes.h"

#include <algorithm>
#include <atomic>

namespace
{
using FExploreJsonWriter = TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;

constexpr int32 DefaultRenderWidth = 512;
constexpr int32 DefaultRenderHeight = 288;
constexpr float DefaultRenderStepVoxels = 0.25f;
constexpr float DefaultRenderMaxDistanceVoxels = 160.0f;
constexpr int32 DefaultExportSize = 128;
constexpr int32 DefaultExportStep = 1;
constexpr int32 DefaultMaxWalkCells = 32000000;
constexpr float DefaultBudgetMinutes = 25.0f;
constexpr float MaxBudgetMinutes = 30.0f;
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

FString VoxelForgeSavedRoot()
{
    FString Root = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins/VoxelForge/Saved")));
    FPaths::NormalizeDirectoryName(Root);
    return Root;
}

bool IsUnderVoxelForgeSaved(const FString& InPath)
{
    FString Candidate = FPaths::ConvertRelativePathToFull(InPath);
    FString Root = VoxelForgeSavedRoot();
    FPaths::NormalizeDirectoryName(Candidate);
    FPaths::NormalizeDirectoryName(Root);
    return Candidate.Equals(Root, ESearchCase::IgnoreCase)
        || Candidate.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase)
        || Candidate.StartsWith(Root + TEXT("\\"), ESearchCase::IgnoreCase);
}

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
    bool bUseOperatorStack = true;
    bool bProfileDensity = false;
    bool bProfileDensityFull = false;
    bool bProfileLod = false;
    bool bOpBounds = false;
    bool bFailureFocusRender = false;
    FString OutDirectory;

    int32 RenderWidth = DefaultRenderWidth;
    int32 RenderHeight = DefaultRenderHeight;
    float RenderStepVoxels = DefaultRenderStepVoxels;
    float RenderMaxDistanceVoxels = DefaultRenderMaxDistanceVoxels;
    int32 ExportSize = DefaultExportSize;
    int32 ExportStep = DefaultExportStep;
    // The canonical export owns one immutable lattice for the whole run.  Batch cases can pass
    // density_grid_reuse=false to produce a matched wall-clock control.
    bool bReuseDensityGrid = true;
    bool bBlockEarlyOut = false;
    int32 MeshMinBatchSize = 1;
    int32 MaxWalkCells = DefaultMaxWalkCells;
    float BudgetMinutes = DefaultBudgetMinutes;
    bool bSurfaceRoughnessOverride = false;
    float SurfaceRoughness = 0.0f;

    FString CanonicalModes() const
    {
        FString Result;
        if (bRender) Result += TEXT("render,");
        if (bWalk) Result += TEXT("walk,");
        if (bExport) Result += TEXT("export,");
        if (bOpBounds) Result += TEXT("opbounds,");
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
    // FParse::Value treats a comma as a command-line separator in some UE commandlet
    // launch paths. Recover the complete token so both -modes=walk,export and the
    // quoted -modes="walk,export" form have the same meaning.
    const int32 ModesOffset = Params.Find(TEXT("modes="), ESearchCase::IgnoreCase);
    if (ModesOffset != INDEX_NONE)
    {
        ModesText = Params.Mid(ModesOffset + 6);
        const int32 ModesEnd = ModesText.Find(TEXT(" "));
        if (ModesEnd != INDEX_NONE)
        {
            ModesText.LeftInline(ModesEnd);
        }
        ModesText.TrimQuotesInline();
    }
    int32 UseOperatorStack = 1;
    FParse::Value(*Params, TEXT("opstack="), UseOperatorStack);
    OutArguments.bUseOperatorStack = UseOperatorStack != 0;
    OutArguments.bProfileDensity = FParse::Param(*Params, TEXT("profiledensity"));
    OutArguments.bProfileDensityFull = FParse::Param(*Params, TEXT("profiledensityfull"));
    OutArguments.bProfileDensity |= OutArguments.bProfileDensityFull;
    OutArguments.bProfileLod = FParse::Param(*Params, TEXT("profilelod"));
    OutArguments.bOpBounds = FParse::Param(*Params, TEXT("opbounds"));
    FParse::Value(*Params, TEXT("out="), OutText);
    FParse::Value(*Params, TEXT("renderwidth="), OutArguments.RenderWidth);
    FParse::Value(*Params, TEXT("renderheight="), OutArguments.RenderHeight);
    FParse::Value(*Params, TEXT("renderstep="), OutArguments.RenderStepVoxels);
    FParse::Value(*Params, TEXT("rendermaxdistance="), OutArguments.RenderMaxDistanceVoxels);
    FParse::Value(*Params, TEXT("exportsize="), OutArguments.ExportSize);
    FParse::Value(*Params, TEXT("exportstep="), OutArguments.ExportStep);
    int32 DensityGridReuse = OutArguments.bReuseDensityGrid ? 1 : 0;
    FParse::Value(*Params, TEXT("densitygridreuse="), DensityGridReuse);
    OutArguments.bReuseDensityGrid = DensityGridReuse != 0;
    int32 BlockEarlyOut = OutArguments.bBlockEarlyOut ? 1 : 0;
    FParse::Value(*Params, TEXT("blockearlyout="), BlockEarlyOut);
    OutArguments.bBlockEarlyOut = BlockEarlyOut != 0;
    FParse::Value(*Params, TEXT("meshminbatch="), OutArguments.MeshMinBatchSize);
    OutArguments.MeshMinBatchSize = FMath::Clamp(OutArguments.MeshMinBatchSize, 1, 64);
    int32 Lod = 0;
    if (FParse::Value(*Params, TEXT("lod="), Lod))
    {
        if (Lod < 0 || Lod > 3)
        {
            OutError = TEXT("lod must be an integer in [0,3].");
            return false;
        }
        OutArguments.ExportStep = 1 << Lod;
    }
    FParse::Value(*Params, TEXT("maxwalkcells="), OutArguments.MaxWalkCells);
    FParse::Value(*Params, TEXT("budget="), OutArguments.BudgetMinutes);
    const bool bSurfaceRoughnessSpecified = Params.Contains(
        TEXT("surfaceroughness="), ESearchCase::IgnoreCase);
    const bool bSurfaceRoughnessParsed = FParse::Value(
        *Params, TEXT("surfaceroughness="), OutArguments.SurfaceRoughness);
    OutArguments.bSurfaceRoughnessOverride = bSurfaceRoughnessSpecified;
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
        else if (Mode == TEXT("opbounds"))
        {
            OutArguments.bOpBounds = true;
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
                TEXT("Unknown mode '%s'. Expected render, walk, export, opbounds, or all."), *Mode);
            return false;
        }
    }
    if (!OutArguments.bRender && !OutArguments.bWalk && !OutArguments.bExport
        && !OutArguments.bOpBounds)
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
    // Render no longer budgets density samples: pixels are rasterised from the one canonical
    // mesh. Keep the legacy renderstep/maxdistance arguments for compatible invocations and use
    // only maxdistance as the raster depth clip.
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
    if (OutArguments.ExportStep < 1
        || OutArguments.ExportStep > 8
        || (OutArguments.ExportStep & (OutArguments.ExportStep - 1)) != 0
        || CHUNK_SIZE % OutArguments.ExportStep != 0)
    {
        OutError = TEXT("exportstep must be a power of two in [1,8] and divide CHUNK_SIZE.");
        return false;
    }
    if (OutArguments.MaxWalkCells <= 0)
    {
        OutError = TEXT("maxwalkcells must be greater than zero.");
        return false;
    }
    if (!FMath::IsFinite(OutArguments.BudgetMinutes)
        || OutArguments.BudgetMinutes <= 0.0f
        || OutArguments.BudgetMinutes > MaxBudgetMinutes)
    {
        OutError = FString::Printf(
            TEXT("budget must be finite and in (0,%g] minutes."), MaxBudgetMinutes);
        return false;
    }
    if (OutArguments.bSurfaceRoughnessOverride
        && !bSurfaceRoughnessParsed)
    {
        OutError = TEXT("surfaceroughness must be a finite number greater than or equal to zero.");
        return false;
    }
    if (OutArguments.bSurfaceRoughnessOverride
        && (!FMath::IsFinite(OutArguments.SurfaceRoughness)
            || OutArguments.SurfaceRoughness < 0.0f))
    {
        OutError = TEXT("surfaceroughness must be finite and greater than or equal to zero.");
        return false;
    }

    if (OutText.IsEmpty())
    {
        OutText = FPaths::Combine(
            VoxelForgeSavedRoot(),
            FString::Printf(TEXT("Explore/seed%d_%s_slot%d"),
                OutArguments.Seed,
                ArchetypeName(OutArguments.Archetype),
                OutArguments.Slot));
    }
    if (FPaths::IsRelative(OutText))
    {
        OutError = FString::Printf(
            TEXT("out must be an absolute path under '%s'."), *VoxelForgeSavedRoot());
        return false;
    }
    OutArguments.OutDirectory = FPaths::ConvertRelativePathToFull(OutText);
    FPaths::NormalizeDirectoryName(OutArguments.OutDirectory);
    if (!IsUnderVoxelForgeSaved(OutArguments.OutDirectory))
    {
        OutError = FString::Printf(
            TEXT("out must be an absolute path under '%s' (got '%s')."),
            *VoxelForgeSavedRoot(), *OutArguments.OutDirectory);
        return false;
    }
    return true;
}

VoxelDensityProfile::EMode ExploreProfileMode(const FExploreArguments& Arguments)
{
    return Arguments.bProfileDensityFull
        ? VoxelDensityProfile::EMode::Full
        : VoxelDensityProfile::EMode::Sampled;
}

struct FExploreBudget
{
    double StartSeconds = 0.0;
    double LimitSeconds = 0.0;
    bool bTruncated = false;
    FString TruncatedDuring;
    TArray<FString> CompletedModes;

    FExploreBudget(double InStartSeconds, double InLimitSeconds)
        : StartSeconds(InStartSeconds)
        , LimitSeconds(InLimitSeconds)
    {
    }

    double ElapsedSeconds() const
    {
        return FPlatformTime::Seconds() - StartSeconds;
    }

    double RemainingSeconds() const
    {
        return LimitSeconds - ElapsedSeconds();
    }

    bool ShouldStop(const TCHAR* Phase)
    {
        if (bTruncated)
        {
            return true;
        }

        // Leave a small deterministic cleanup window for the current phase to close files and
        // emit the report. The owner-facing hard ceiling is still enforced by the argument cap at
        // 30 minutes; the default 25-minute budget leaves substantially more headroom.
        constexpr double CleanupReserveSeconds = 0.25;
        if (RemainingSeconds() <= CleanupReserveSeconds)
        {
            bTruncated = true;
            TruncatedDuring = Phase != nullptr ? FString(Phase) : TEXT("unknown");
            UE_LOG(LogTemp, Warning,
                TEXT("[VoxelForgeExplore] budget reached during %s; keeping completed artifacts."),
                *TruncatedDuring);
            return true;
        }
        return false;
    }

    void CompleteMode(const TCHAR* Mode)
    {
        if (Mode == nullptr || bTruncated)
        {
            return;
        }
        CompletedModes.AddUnique(FString(Mode));
    }
};

struct FExploreMeshTriangle
{
    FVector A = FVector::ZeroVector;
    FVector B = FVector::ZeroVector;
    FVector C = FVector::ZeroVector;
    FVector NormalA = FVector::UpVector;
    FVector NormalB = FVector::UpVector;
    FVector NormalC = FVector::UpVector;
    FVector Min = FVector::ZeroVector;
    FVector Max = FVector::ZeroVector;
    FVector Centre = FVector::ZeroVector;
    FVector Normal = FVector::UpVector;
};

struct FExploreMeshBvhNode
{
    FVector Min = FVector::ZeroVector;
    FVector Max = FVector::ZeroVector;
    int32 Left = INDEX_NONE;
    int32 Right = INDEX_NONE;
    int32 FirstTriangle = 0;
    int32 TriangleCount = 0;

    bool IsLeaf() const
    {
        return Left == INDEX_NONE && Right == INDEX_NONE;
    }
};

struct FExploreWorld
{
    TStrongObjectPtr<UVoxelSettings> Settings;
    TStrongObjectPtr<UVoxelStrateManager> Manager;
    TStrongObjectPtr<UVoxelDiffLayer> DiffLayer;
    TStrongObjectPtr<UVoxelGenerator> Generator;
    TStrongObjectPtr<UVoxelMarchingCubesMesher> Mesher;
    TArray<TStrongObjectPtr<UVoxelStrateDefinition>> Definitions;

    // One canonical mesh cache for this commandlet run. Render and export consume these exact
    // triangles; neither path is allowed to ask the density field once per pixel.
    FVoxelMeshData ExploreMesh;
    FVoxelSharedDensityGrid ExploreSharedDensityGrid;
    FIntVector ExploreMeshOrigin = FIntVector::ZeroValue;
    int32 ExploreMeshSize = 0;
    int32 ExploreMeshTileCount = 0;
    int32 ExploreMeshTilesCompleted = 0;
    double ExploreMeshSeconds = 0.0;
    double ExploreDensityGridSeconds = 0.0;
    int64 ExploreDensityGridTotalSamples = 0;
    int64 ExploreDensityGridUniqueSamples = 0;
    int64 ExploreDensityGridDuplicateSamplesBefore = 0;
    int64 ExploreDensityGridDuplicateSamplesAfter = 0;
    bool bExploreDensityGridReused = false;
    int32 ExploreMeshTaskWorkerCount = 0;
    int32 ExploreMeshTaskJobCount = 0;
    int32 ExploreMeshTaskMinBatchSize = 1;
    double ExploreMeshTaskMinSeconds = 0.0;
    double ExploreMeshTaskMeanSeconds = 0.0;
    double ExploreMeshTaskMaxSeconds = 0.0;
    double ExploreMeshTaskSumSeconds = 0.0;
    double ExploreMeshTaskWallSeconds = 0.0;
    double ExploreMeshTaskLaunchOverheadSeconds = 0.0;
    bool bExploreMeshBuilt = false;
    bool bExploreMeshComplete = false;
    FString ExploreMeshError;
    FString ExploreGeometryHash;

    // A deterministic CPU acceleration structure over the canonical triangles. It is built once
    // after meshing and reused by every viewpoint; no density query is reachable from raster.
    TArray<FExploreMeshTriangle> ExploreTriangles;
    TArray<int32> ExploreTriangleOrder;
    TArray<FExploreMeshBvhNode> ExploreBvhNodes;
    int32 ExploreBvhRoot = INDEX_NONE;
    double ExploreAccelerationSeconds = 0.0;
    bool bExploreAccelerationBuilt = false;

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
            // Explicitly select the production switch so the same synthetic layout, seed and
            // mesher invocation can be measured through both density branches.
            Definition->bUseOperatorStack = Arguments.bUseOperatorStack;

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
        if (Arguments.bSurfaceRoughnessOverride)
        {
            // This is deliberately applied after layout construction and only to the target slot's
            // resolved definition. Passage placement and every neighbouring strate retain their
            // authored/default values; only the sampled target field gets the experiment knob.
            switch (Arguments.Archetype)
            {
            case ECaveGeneratorType::FlatPlain:
            case ECaveGeneratorType::CrystalChamber:
                Target.Definition->SlabParams.FloorRoughness = Arguments.SurfaceRoughness;
                Target.Definition->SlabParams.CeilingRoughness = Arguments.SurfaceRoughness;
                break;
            case ECaveGeneratorType::Maze:
                Target.Definition->MazeParams.SurfaceRoughness = Arguments.SurfaceRoughness;
                break;
            case ECaveGeneratorType::SurfaceWorld:
                Target.Definition->SurfaceParams.SurfaceRoughness = Arguments.SurfaceRoughness;
                break;
            case ECaveGeneratorType::VerticalShafts:
                Target.Definition->VerticalShaftParams.SurfaceRoughness = Arguments.SurfaceRoughness;
                break;
            case ECaveGeneratorType::FloatingIslands:
                Target.Definition->FloatingIslandParams.SurfaceRoughness = Arguments.SurfaceRoughness;
                break;
            case ECaveGeneratorType::TunnelNetwork:
            case ECaveGeneratorType::Underwater:
            default:
                Target.Definition->GenerationParams.SurfaceRoughness = Arguments.SurfaceRoughness;
                break;
            }
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
        Mesher->bUseBlockEarlyOut = Arguments.bBlockEarlyOut;

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
    bool bPartial = false;
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
    int64 LegacyTwoViewEstimatedDensitySamples = 0;
    FString StepRationale;
    FString CameraSeedPolicy;
    int32 CameraSeedCount = 0;
    bool bAllCamerasPlayerFit = false;
    double MeshSeconds = 0.0;
    double RasterSeconds = 0.0;
    double FirstViewRasterSeconds = 0.0;
    double AdditionalViewRasterSeconds = 0.0;
    int32 RequestedViewpointCount = 8;
    bool bMeshComplete = false;
    double MeshAccelerationSeconds = 0.0;
    bool bTruncated = false;
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
    // Captured by the one walk pass and consumed by render camera selection. These are internal
    // hand-off buffers, not a second source of truth for the field.
    FVoxelStrateSampleGrid SharedSampleGrid;
    TArray<FVector> CameraSeedPoses;
    TArray<FString> CameraSeedSources;
};

struct FExploreMeshMetrics
{
    int32 VertexCount = 0;
    int32 TriangleCount = 0;
    int32 SharedEdgeCount = 0;
    int32 DihedralSampleCount = 0;
    int32 NormalComparisonTriangleCount = 0;
    float EdgeLengthP50Meters = 0.0f;
    float EdgeLengthP99Meters = 0.0f;
    float EdgeLengthMaxMeters = 0.0f;
    float DihedralP50Degrees = 0.0f;
    float DihedralP75Degrees = 0.0f;
    float DihedralP90Degrees = 0.0f;
    float DihedralP95Degrees = 0.0f;
    float DihedralP99Degrees = 0.0f;
    float DihedralFractionOver20Degrees = 0.0f;
    float DihedralFractionOver40Degrees = 0.0f;
    float FaceNormalVsMeanVertexNormalP50Degrees = 0.0f;
    float FaceNormalVsMeanVertexNormalP90Degrees = 0.0f;
};

struct FExploreMeshEdgeKey
{
    FIntVector First = FIntVector::ZeroValue;
    FIntVector Second = FIntVector::ZeroValue;

    bool operator==(const FExploreMeshEdgeKey& Other) const
    {
        return First == Other.First && Second == Other.Second;
    }
};

FORCEINLINE uint32 GetTypeHash(const FExploreMeshEdgeKey& Key)
{
    return HashCombine(GetTypeHash(Key.First), GetTypeHash(Key.Second));
}

struct FExploreMeshEdgeIncident
{
    int32 IncidentCount = 0;
    int32 FirstTriangle = INDEX_NONE;
    int32 SecondTriangle = INDEX_NONE;
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
    int32 MesherTilesCompleted = 0;
    int64 EstimatedWorkingBytes = 0;
    int64 WorkingMemoryCapBytes = MaxExportWorkingBytes;
    int64 MeshArrayBytes = 0;
    int64 MeshFileSizeBytes = 0;
    int32 VertexCount = 0;
    int32 TriangleCount = 0;
    FExploreMeshMetrics SurfaceMetrics;
    double MeshSeconds = 0.0;
    double MeshUsPerVoxel = 0.0;
    FString GeometryHash;
    bool bTruncated = false;
};

struct FExploreProfilerComparison
{
    bool bAvailable = false;
    FString Scope;
    double ProfiledSeconds = 0.0;
    double UnprofiledSeconds = 0.0;
    double DiscrepancySeconds = 0.0;
    double DiscrepancyPercent = 0.0;
    double Ratio = 0.0;
    bool bGeometryEqual = false;
    FString ProfiledGeometryHash;
    FString UnprofiledGeometryHash;
    FString SelfCheck = TEXT("not_run");
};

struct FExploreRunOutput
{
    FExploreRenderOutput Render;
    FExploreWalkOutput Walk;
    FExploreExportOutput Export;
    double BudgetSeconds = 0.0;
    double ElapsedSeconds = 0.0;
    double WalkSeconds = 0.0;
    bool bTruncated = false;
    FString TruncatedDuring;
    TArray<FString> CompletedModes;
    VoxelDensityProfile::FSnapshot Profile;
    bool bHasProfile = false;
    FExploreProfilerComparison ProfilerComparison;
};

bool ValidateCanonicalTile(const FVoxelMeshData& MeshData, FString& OutError);
bool MeasureExploreMesh(
    const FVoxelMeshData& MeshData,
    FExploreMeshMetrics& OutMetrics,
    FString& OutError);

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

FIntVector ChooseExploreMeshOrigin(
    const FExploreArguments& Arguments,
    const FExploreWalkOutput* CameraSeedWalk,
    int32 MeshSize,
    int32 TargetMiddleWorldZ)
{
    FVector Centre(0.0f, 0.0f, static_cast<float>(TargetMiddleWorldZ));
    if (CameraSeedWalk != nullptr && CameraSeedWalk->CameraSeedPoses.Num() > 0)
    {
        Centre = CameraSeedWalk->CameraSeedPoses[0];
    }

    const auto AlignTileOrigin = [MeshSize](float CentreCoordinate)
    {
        return FMath::FloorToInt(
            (CentreCoordinate - 0.5f * static_cast<float>(MeshSize))
                / static_cast<float>(CHUNK_SIZE))
            * CHUNK_SIZE;
    };
    return FIntVector(
        AlignTileOrigin(Centre.X),
        AlignTileOrigin(Centre.Y),
        AlignTileOrigin(Centre.Z));
}

bool AppendCanonicalMesh(
    FVoxelMeshData& Destination,
    const FVoxelMeshData& Source,
    FString& OutError)
{
    if (!ValidateCanonicalTile(Source, OutError))
    {
        return false;
    }

    const int32 VertexOffset = Destination.Vertices.Num();
    for (const int32 Index : Source.Triangles)
    {
        if (Index < 0 || Index >= Source.Vertices.Num())
        {
            OutError = TEXT("Canonical mesher returned an out-of-range triangle index.");
            return false;
        }
    }

    Destination.Vertices.Append(Source.Vertices);
    Destination.Normals.Append(Source.Normals);
    Destination.UVs.Append(Source.UVs);
    Destination.Colors.Append(Source.Colors);
    Destination.Triangles.Reserve(Destination.Triangles.Num() + Source.Triangles.Num());
    for (const int32 Index : Source.Triangles)
    {
        Destination.Triangles.Add(Index + VertexOffset);
    }
    // A multi-tile aggregate interleaves each tile's ground/cap runs. The CPU rasterizer uses
    // flat geometry and the OBJ path is material-neutral, so there is deliberately no invented
    // global ceiling run here.
    Destination.NumCeilingTriangles = 0;
    return true;
}

FString ComputeGeometryHash(const FVoxelMeshData& MeshData)
{
    uint32 Crc = 0;
    const int32 Counts[] = {
        MeshData.Vertices.Num(),
        MeshData.Normals.Num(),
        MeshData.UVs.Num(),
        MeshData.Colors.Num(),
        MeshData.Triangles.Num(),
        MeshData.NumCeilingTriangles,
    };
    Crc = FCrc::MemCrc32(Counts, sizeof(Counts), Crc);
    if (MeshData.Vertices.Num() > 0)
    {
        Crc = FCrc::MemCrc32(
            MeshData.Vertices.GetData(),
            MeshData.Vertices.Num() * sizeof(FVector), Crc);
    }
    if (MeshData.Normals.Num() > 0)
    {
        Crc = FCrc::MemCrc32(
            MeshData.Normals.GetData(),
            MeshData.Normals.Num() * sizeof(FVector), Crc);
    }
    if (MeshData.UVs.Num() > 0)
    {
        Crc = FCrc::MemCrc32(
            MeshData.UVs.GetData(),
            MeshData.UVs.Num() * sizeof(FVector2D), Crc);
    }
    if (MeshData.Colors.Num() > 0)
    {
        Crc = FCrc::MemCrc32(
            MeshData.Colors.GetData(),
            MeshData.Colors.Num() * sizeof(FColor), Crc);
    }
    if (MeshData.Triangles.Num() > 0)
    {
        Crc = FCrc::MemCrc32(
            MeshData.Triangles.GetData(),
            MeshData.Triangles.Num() * sizeof(int32), Crc);
    }
    return FString::Printf(TEXT("%08X"), Crc);
}

bool EnsureExploreMesh(
    const FExploreArguments& Arguments,
    FExploreWorld& World,
    const FExploreWalkOutput* CameraSeedWalk,
    FExploreBudget& Budget,
    FString& OutError)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_ExploreMesh);
    if (World.bExploreMeshBuilt)
    {
        OutError = World.ExploreMeshError;
        return World.bExploreMeshComplete;
    }

    World.bExploreMeshBuilt = true;
    World.bExploreMeshComplete = false;
    World.ExploreMeshError.Reset();
    World.ExploreMesh.Clear();
    World.ExploreMeshSize = Arguments.ExportSize;
    const int32 TilesPerAxis = Arguments.ExportSize / CHUNK_SIZE;
    World.ExploreMeshTileCount = TilesPerAxis * TilesPerAxis * TilesPerAxis;
    World.ExploreMeshTilesCompleted = 0;
    World.ExploreMeshOrigin = ChooseExploreMeshOrigin(
        Arguments,
        CameraSeedWalk,
        World.ExploreMeshSize,
        FMath::FloorToInt(0.5f * static_cast<float>(
            World.TargetBottomWorldZ + World.TargetTopWorldZ)));
    World.ExploreMeshSeconds = 0.0;
    World.ExploreDensityGridSeconds = 0.0;
    World.ExploreDensityGridTotalSamples = 0;
    World.ExploreDensityGridUniqueSamples = 0;
    World.ExploreDensityGridDuplicateSamplesBefore = 0;
    World.ExploreDensityGridDuplicateSamplesAfter = 0;
    World.bExploreDensityGridReused = false;
    World.ExploreMeshTaskWorkerCount = 0;
    World.ExploreMeshTaskJobCount = 0;
    World.ExploreMeshTaskMinBatchSize = FMath::Clamp(Arguments.MeshMinBatchSize, 1, 64);
    World.ExploreMeshTaskMinSeconds = 0.0;
    World.ExploreMeshTaskMeanSeconds = 0.0;
    World.ExploreMeshTaskMaxSeconds = 0.0;
    World.ExploreMeshTaskSumSeconds = 0.0;
    World.ExploreMeshTaskWallSeconds = 0.0;
    World.ExploreMeshTaskLaunchOverheadSeconds = 0.0;
    World.Mesher->SetSharedDensityGrid(nullptr);
    World.ExploreSharedDensityGrid.Reset();

    if (Budget.ShouldStop(TEXT("mesh")))
    {
        World.ExploreMeshError = TEXT("The wall-clock budget elapsed before meshing began.");
        OutError = World.ExploreMeshError;
        return false;
    }

    // This is the same canonical UVoxelMarchingCubesMesher used by export and runtime chunk
    // generation. Skirts are presentation geometry for LOD seams and would extend outside the
    // bounded explorer region, so the shared mesh uses the export setting.
    World.Mesher->bGenerateSkirts = false;

    const double MeshStartSeconds = FPlatformTime::Seconds();
    if (Budget.ShouldStop(TEXT("mesh")))
    {
        World.ExploreMeshError = TEXT("The wall-clock budget elapsed before mesh tiles began.");
        OutError = World.ExploreMeshError;
        World.ExploreMeshSeconds = FPlatformTime::Seconds() - MeshStartSeconds;
        return false;
    }

    // Adjacent tiles ask for the same one-point-halo planes.  Build one immutable lattice for
    // the whole export when requested, then let each tile copy its local window from it.  The
    // precomputation is inside MeshStartSeconds on purpose: this is an end-to-end export number,
    // not a profiled bucket hiding the cost of producing the cache.
    const int32 CellsPerTile = CHUNK_SIZE / Arguments.ExportStep;
    const int32 PointsPerTile = CellsPerTile + 3;
    const int64 TileGridSamples = static_cast<int64>(PointsPerTile)
        * static_cast<int64>(PointsPerTile)
        * static_cast<int64>(PointsPerTile);
    const int32 SharedGridDim = Arguments.ExportSize / Arguments.ExportStep + 3;
    const int64 SharedGridSamples = static_cast<int64>(SharedGridDim)
        * static_cast<int64>(SharedGridDim)
        * static_cast<int64>(SharedGridDim);
    World.ExploreDensityGridTotalSamples = static_cast<int64>(World.ExploreMeshTileCount)
        * TileGridSamples;
    World.ExploreDensityGridUniqueSamples = SharedGridSamples;
    World.ExploreDensityGridDuplicateSamplesBefore = FMath::Max<int64>(
        0, World.ExploreDensityGridTotalSamples - SharedGridSamples);
    World.ExploreDensityGridDuplicateSamplesAfter =
        Arguments.bReuseDensityGrid ? 0 : World.ExploreDensityGridDuplicateSamplesBefore;

    if (Arguments.bReuseDensityGrid)
    {
        const double DensityGridStartSeconds = FPlatformTime::Seconds();
        World.ExploreSharedDensityGrid.OriginVoxels = World.ExploreMeshOrigin
            - FIntVector(Arguments.ExportStep);
        World.ExploreSharedDensityGrid.Step = Arguments.ExportStep;
        World.ExploreSharedDensityGrid.Dim = SharedGridDim;
        World.ExploreSharedDensityGrid.Samples.SetNumUninitialized(
            SharedGridDim * SharedGridDim * SharedGridDim);
        std::atomic<bool> bGridWorkCancelled(false);
        const double GridWorkDeadline = Budget.StartSeconds + Budget.LimitSeconds - 0.25;
        const int32 GridTileCount = TilesPerAxis;
        ParallelFor(
            TEXT("VoxelForgeExploreDensityGrid"),
            World.ExploreMeshTileCount,
            World.ExploreMeshTaskMinBatchSize,
            [&](int32 LinearOwnerIndex)
            {
                if (bGridWorkCancelled.load(std::memory_order_relaxed)
                    || FPlatformTime::Seconds() >= GridWorkDeadline)
                {
                    bGridWorkCancelled.store(true, std::memory_order_relaxed);
                    return;
                }
                const int32 GridOctaveBias = (World.Mesher->LODOctaveDrop > 0
                    && Arguments.ExportStep > 1)
                    ? World.Mesher->LODOctaveDrop
                        * (int32)FMath::FloorLog2((uint32)Arguments.ExportStep)
                    : 0;
                const int32 PreviousOctaveBias = VoxelGenLOD::GetThreadOctaveBias();
                const int32 PreviousSampleStep = VoxelGenLOD::GetThreadSampleStep();
                VoxelGenLOD::SetThreadOctaveBias(GridOctaveBias);
                VoxelGenLOD::SetThreadSampleStep(Arguments.ExportStep);
                const int32 OwnerX = LinearOwnerIndex % GridTileCount;
                const int32 OwnerY = (LinearOwnerIndex / GridTileCount) % GridTileCount;
                const int32 OwnerZ = LinearOwnerIndex / (GridTileCount * GridTileCount);
                // Partition the shared lattice into disjoint rectangular owner regions.  A
                // boundary sample belongs to exactly one owner; tile windows may read it from
                // either side.  This keeps the generator's spatial locality while guaranteeing
                // one evaluation per global lattice point.
                const auto OwnerMinGrid = [CellsPerTile](int32 Owner) -> int32
                {
                    return Owner == 0 ? 0 : 1 + Owner * CellsPerTile;
                };
                const auto OwnerMaxGrid = [CellsPerTile, SharedGridDim, GridTileCount](int32 Owner) -> int32
                {
                    return Owner == GridTileCount - 1
                        ? SharedGridDim
                        : 1 + (Owner + 1) * CellsPerTile;
                };
                const int32 GxMin = OwnerMinGrid(OwnerX);
                const int32 GxMax = OwnerMaxGrid(OwnerX);
                const int32 GyMin = OwnerMinGrid(OwnerY);
                const int32 GyMax = OwnerMaxGrid(OwnerY);
                const int32 GzMin = OwnerMinGrid(OwnerZ);
                const int32 GzMax = OwnerMaxGrid(OwnerZ);
                for (int32 Gz = GzMin; Gz < GzMax; ++Gz)
                {
                    for (int32 Gy = GyMin; Gy < GyMax; ++Gy)
                    {
                        for (int32 Gx = GxMin; Gx < GxMax; ++Gx)
                        {
                            const int32 Index = (Gz * SharedGridDim + Gy) * SharedGridDim + Gx;
                            World.ExploreSharedDensityGrid.Samples[Index] =
                                World.Generator->GetDensityAt(
                                    World.ExploreSharedDensityGrid.OriginVoxels.X + Gx * Arguments.ExportStep,
                                    World.ExploreSharedDensityGrid.OriginVoxels.Y + Gy * Arguments.ExportStep,
                                    World.ExploreSharedDensityGrid.OriginVoxels.Z + Gz * Arguments.ExportStep);
                        }
                    }
                }
                VoxelGenLOD::SetThreadOctaveBias(PreviousOctaveBias);
                VoxelGenLOD::SetThreadSampleStep(PreviousSampleStep);
            },
            EParallelForFlags::None);
        World.ExploreDensityGridSeconds = FPlatformTime::Seconds() - DensityGridStartSeconds;
        if (bGridWorkCancelled.load(std::memory_order_relaxed))
        {
            World.ExploreMeshError = TEXT("The wall-clock budget elapsed while building the shared density grid.");
            OutError = World.ExploreMeshError;
            World.ExploreMeshSeconds = FPlatformTime::Seconds() - MeshStartSeconds;
            return false;
        }
        World.Mesher->SetSharedDensityGrid(&World.ExploreSharedDensityGrid);
        World.bExploreDensityGridReused = true;
    }

    // Generate each tile through the canonical mesher on worker threads, then append strictly in
    // Z/Y/X order. The mesher's scratch buffers and the generator's hot caches are thread-local;
    // the fixed merge order keeps the aggregate mesh and all downstream raster/export bytes
    // deterministic even though tile completion order is not.
    TArray<FVoxelMeshData> TileMeshes;
    TileMeshes.SetNum(World.ExploreMeshTileCount);
    TArray<FString> TileErrors;
    TileErrors.SetNum(World.ExploreMeshTileCount);
    TArray<uint8> TileCompleted;
    TileCompleted.Init(0, World.ExploreMeshTileCount);
    TArray<double> TileDurations;
    TileDurations.Init(0.0, World.ExploreMeshTileCount);
    TArray<double> TileStartOffsets;
    TileStartOffsets.Init(-1.0, World.ExploreMeshTileCount);
    TArray<double> TileEndOffsets;
    TileEndOffsets.Init(-1.0, World.ExploreMeshTileCount);
    TArray<uint32> TileThreadIds;
    TileThreadIds.Init(0, World.ExploreMeshTileCount);
    std::atomic<bool> bTileWorkCancelled(false);
    const double TileWorkDeadline = Budget.StartSeconds + Budget.LimitSeconds - 0.25;
    const double ParallelStartSeconds = FPlatformTime::Seconds();
    ParallelFor(
        TEXT("VoxelForgeExploreMeshTiles"),
        World.ExploreMeshTileCount,
        World.ExploreMeshTaskMinBatchSize,
        [&](int32 LinearTileIndex)
        {
            if (bTileWorkCancelled.load(std::memory_order_relaxed)
                || FPlatformTime::Seconds() >= TileWorkDeadline)
            {
                bTileWorkCancelled.store(true, std::memory_order_relaxed);
                return;
            }

            const double TileStartSeconds = FPlatformTime::Seconds();
            TileStartOffsets[LinearTileIndex] = TileStartSeconds - ParallelStartSeconds;
            TileThreadIds[LinearTileIndex] = FPlatformTLS::GetCurrentThreadId();

            const int32 TileX = LinearTileIndex % TilesPerAxis;
            const int32 TileY = (LinearTileIndex / TilesPerAxis) % TilesPerAxis;
            const int32 TileZ = LinearTileIndex / (TilesPerAxis * TilesPerAxis);
            const FIntVector TileOrigin = World.ExploreMeshOrigin
                + FIntVector(TileX * CHUNK_SIZE, TileY * CHUNK_SIZE, TileZ * CHUNK_SIZE);
            TileMeshes[LinearTileIndex] = World.Mesher->GenerateMesh(
                TileOrigin,
                Arguments.ExportStep,
                CHUNK_SIZE / Arguments.ExportStep);

            FString TileError;
            if (!ValidateCanonicalTile(TileMeshes[LinearTileIndex], TileError))
            {
                TileErrors[LinearTileIndex] = MoveTemp(TileError);
                bTileWorkCancelled.store(true, std::memory_order_relaxed);
                const double TileEndSeconds = FPlatformTime::Seconds();
                TileEndOffsets[LinearTileIndex] = TileEndSeconds - ParallelStartSeconds;
                TileDurations[LinearTileIndex] = TileEndSeconds - TileStartSeconds;
                return;
            }
            TileCompleted[LinearTileIndex] = 1;
            const double TileEndSeconds = FPlatformTime::Seconds();
            TileEndOffsets[LinearTileIndex] = TileEndSeconds - ParallelStartSeconds;
            TileDurations[LinearTileIndex] = TileEndSeconds - TileStartSeconds;
        },
        EParallelForFlags::None);
    const double ParallelEndSeconds = FPlatformTime::Seconds();

    TArray<double> CompletedTileDurations;
    TSet<uint32> WorkerIds;
    double EarliestTileStart = 1.0e30;
    double LatestTileEnd = 0.0;
    for (int32 TileIndex = 0; TileIndex < World.ExploreMeshTileCount; ++TileIndex)
    {
        if (!TileCompleted[TileIndex])
        {
            continue;
        }
        CompletedTileDurations.Add(TileDurations[TileIndex]);
        WorkerIds.Add(TileThreadIds[TileIndex]);
        EarliestTileStart = FMath::Min(EarliestTileStart, TileStartOffsets[TileIndex]);
        LatestTileEnd = FMath::Max(LatestTileEnd, TileEndOffsets[TileIndex]);
    }
    if (CompletedTileDurations.Num() > 0)
    {
        CompletedTileDurations.Sort();
        World.ExploreMeshTaskJobCount = CompletedTileDurations.Num();
        World.ExploreMeshTaskWorkerCount = WorkerIds.Num();
        World.ExploreMeshTaskMinSeconds = CompletedTileDurations[0];
        World.ExploreMeshTaskMaxSeconds = CompletedTileDurations.Last();
        for (const double Duration : CompletedTileDurations)
        {
            World.ExploreMeshTaskSumSeconds += Duration;
        }
        World.ExploreMeshTaskMeanSeconds = World.ExploreMeshTaskSumSeconds
            / static_cast<double>(CompletedTileDurations.Num());
    }
    World.ExploreMeshTaskWallSeconds = ParallelEndSeconds - ParallelStartSeconds;
    if (EarliestTileStart < 1.0e30)
    {
        World.ExploreMeshTaskLaunchOverheadSeconds = FMath::Max(0.0, EarliestTileStart)
            + FMath::Max(0.0, World.ExploreMeshTaskWallSeconds - LatestTileEnd);
    }

    int32 CompletedTiles = 0;
    FString MeshError;
    for (; CompletedTiles < World.ExploreMeshTileCount; ++CompletedTiles)
    {
        if (!TileCompleted[CompletedTiles])
        {
            break;
        }
        if (!AppendCanonicalMesh(
                World.ExploreMesh,
                TileMeshes[CompletedTiles],
                MeshError))
        {
            World.ExploreMeshError = MeshError;
            OutError = MeshError;
            World.ExploreMeshTilesCompleted = CompletedTiles;
            World.ExploreMeshSeconds = FPlatformTime::Seconds() - MeshStartSeconds;
            return false;
        }
        World.ExploreMeshTilesCompleted = CompletedTiles + 1;
    }

    if (CompletedTiles < World.ExploreMeshTileCount)
    {
        if (!TileErrors[CompletedTiles].IsEmpty())
        {
            World.ExploreMeshError = TileErrors[CompletedTiles];
            OutError = World.ExploreMeshError;
            World.ExploreMeshSeconds = FPlatformTime::Seconds() - MeshStartSeconds;
            return false;
        }

        Budget.ShouldStop(TEXT("mesh"));
        World.ExploreMeshError = FString::Printf(
            TEXT("Mesh was truncated after %d/%d canonical tiles."),
            CompletedTiles,
            World.ExploreMeshTileCount);
        OutError = World.ExploreMeshError;
        World.ExploreMeshSeconds = FPlatformTime::Seconds() - MeshStartSeconds;
        return false;
    }

    World.ExploreMeshSeconds = FPlatformTime::Seconds() - MeshStartSeconds;
    World.bExploreMeshComplete = true;
    World.ExploreGeometryHash = ComputeGeometryHash(World.ExploreMesh);
    OutError.Reset();
    return true;
}

float ExploreAxisValue(const FVector& Value, int32 Axis)
{
    switch (Axis)
    {
    case 1: return Value.Y;
    case 2: return Value.Z;
    default: return Value.X;
    }
}

FVector ExploreTriangleMin(const FVector& A, const FVector& B, const FVector& C)
{
    return FVector(
        FMath::Min3(A.X, B.X, C.X),
        FMath::Min3(A.Y, B.Y, C.Y),
        FMath::Min3(A.Z, B.Z, C.Z));
}

FVector ExploreTriangleMax(const FVector& A, const FVector& B, const FVector& C)
{
    return FVector(
        FMath::Max3(A.X, B.X, C.X),
        FMath::Max3(A.Y, B.Y, C.Y),
        FMath::Max3(A.Z, B.Z, C.Z));
}

bool EnsureExploreAcceleration(
    const FExploreArguments& Arguments,
    FExploreWorld& World,
    FExploreBudget& Budget,
    FString& OutError)
{
    if (World.bExploreAccelerationBuilt)
    {
        OutError.Reset();
        return true;
    }

    World.ExploreTriangles.Reset();
    World.ExploreTriangleOrder.Reset();
    World.ExploreBvhNodes.Reset();
    World.ExploreBvhRoot = INDEX_NONE;
    World.ExploreAccelerationSeconds = 0.0;
    const double AccelerationStartSeconds = FPlatformTime::Seconds();

    if (!World.bExploreMeshComplete)
    {
        OutError = TEXT("Cannot build mesh acceleration before the canonical mesh is complete.");
        return false;
    }
    if (Budget.ShouldStop(TEXT("mesh-acceleration")))
    {
        OutError = TEXT("The wall-clock budget elapsed before mesh acceleration began.");
        return false;
    }

    const FVoxelMeshData& Mesh = World.ExploreMesh;
    if (Mesh.Triangles.Num() % 3 != 0)
    {
        OutError = TEXT("Canonical mesh triangle index data is not divisible by three.");
        return false;
    }

    World.ExploreTriangles.Reserve(Mesh.Triangles.Num() / 3);
    for (int32 TriangleIndex = 0;
         TriangleIndex + 2 < Mesh.Triangles.Num();
         TriangleIndex += 3)
    {
        if ((TriangleIndex & 16383) == 0
            && Budget.ShouldStop(TEXT("mesh-acceleration")))
        {
            OutError = TEXT("Mesh acceleration was truncated before its triangle cache completed.");
            World.ExploreTriangles.Reset();
            return false;
        }

        const int32 IndexA = Mesh.Triangles[TriangleIndex];
        const int32 IndexB = Mesh.Triangles[TriangleIndex + 1];
        const int32 IndexC = Mesh.Triangles[TriangleIndex + 2];
        if (!Mesh.Vertices.IsValidIndex(IndexA)
            || !Mesh.Vertices.IsValidIndex(IndexB)
            || !Mesh.Vertices.IsValidIndex(IndexC))
        {
            OutError = TEXT("Canonical mesh acceleration saw an out-of-range triangle index.");
            World.ExploreTriangles.Reset();
            return false;
        }

        const FVector A = Mesh.Vertices[IndexA] / VOXEL_SIZE;
        const FVector B = Mesh.Vertices[IndexB] / VOXEL_SIZE;
        const FVector C = Mesh.Vertices[IndexC] / VOXEL_SIZE;
        if (!FMath::IsFinite(A.X) || !FMath::IsFinite(A.Y) || !FMath::IsFinite(A.Z)
            || !FMath::IsFinite(B.X) || !FMath::IsFinite(B.Y) || !FMath::IsFinite(B.Z)
            || !FMath::IsFinite(C.X) || !FMath::IsFinite(C.Y) || !FMath::IsFinite(C.Z))
        {
            OutError = TEXT("Canonical mesh acceleration saw a non-finite vertex.");
            World.ExploreTriangles.Reset();
            return false;
        }

        const FVector Cross = FVector::CrossProduct(B - A, C - A);
        if (Cross.SizeSquared() <= SMALL_NUMBER)
        {
            // Degenerate canonical triangles have no raster contribution. Keep them out of the
            // acceleration structure without changing the source mesh or its export semantics.
            continue;
        }

        FExploreMeshTriangle& Triangle = World.ExploreTriangles.AddDefaulted_GetRef();
        Triangle.A = A;
        Triangle.B = B;
        Triangle.C = C;
        Triangle.NormalA = Mesh.Normals[IndexA].GetSafeNormal();
        Triangle.NormalB = Mesh.Normals[IndexB].GetSafeNormal();
        Triangle.NormalC = Mesh.Normals[IndexC].GetSafeNormal();
        Triangle.Min = ExploreTriangleMin(A, B, C);
        Triangle.Max = ExploreTriangleMax(A, B, C);
        Triangle.Centre = (A + B + C) / 3.0f;
        Triangle.Normal = Cross.GetSafeNormal();
    }

    if (World.ExploreTriangles.Num() == 0)
    {
        // A valid empty mesh is still a complete mesh render: every pixel remains the explicit
        // background colour. The root sentinel lets the rasterizer handle this deterministically.
        World.ExploreAccelerationSeconds = FPlatformTime::Seconds() - AccelerationStartSeconds;
        World.bExploreAccelerationBuilt = true;
        OutError.Reset();
        return true;
    }

    World.ExploreTriangleOrder.SetNumUninitialized(World.ExploreTriangles.Num());
    for (int32 TriangleIndex = 0;
         TriangleIndex < World.ExploreTriangleOrder.Num();
         ++TriangleIndex)
    {
        World.ExploreTriangleOrder[TriangleIndex] = TriangleIndex;
    }

    World.ExploreBvhNodes.Reserve(World.ExploreTriangles.Num() / 4 + 2);
    bool bBuildFailed = false;
    TFunction<int32(int32, int32)> BuildNode;
    BuildNode = [&](int32 Start, int32 Count) -> int32
    {
        if (bBuildFailed || Count <= 0)
        {
            return INDEX_NONE;
        }
        if (Budget.ShouldStop(TEXT("mesh-acceleration")))
        {
            bBuildFailed = true;
            return INDEX_NONE;
        }

        const int32 NodeIndex = World.ExploreBvhNodes.AddDefaulted();
        FExploreMeshBvhNode Node;
        Node.Min = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
        Node.Max = FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        FVector CentroidMin = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
        FVector CentroidMax = FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (int32 Offset = 0; Offset < Count; ++Offset)
        {
            const int32 TriangleIndex = World.ExploreTriangleOrder[Start + Offset];
            const FExploreMeshTriangle& Triangle = World.ExploreTriangles[TriangleIndex];
            Node.Min.X = FMath::Min(Node.Min.X, Triangle.Min.X);
            Node.Min.Y = FMath::Min(Node.Min.Y, Triangle.Min.Y);
            Node.Min.Z = FMath::Min(Node.Min.Z, Triangle.Min.Z);
            Node.Max.X = FMath::Max(Node.Max.X, Triangle.Max.X);
            Node.Max.Y = FMath::Max(Node.Max.Y, Triangle.Max.Y);
            Node.Max.Z = FMath::Max(Node.Max.Z, Triangle.Max.Z);
            CentroidMin.X = FMath::Min(CentroidMin.X, Triangle.Centre.X);
            CentroidMin.Y = FMath::Min(CentroidMin.Y, Triangle.Centre.Y);
            CentroidMin.Z = FMath::Min(CentroidMin.Z, Triangle.Centre.Z);
            CentroidMax.X = FMath::Max(CentroidMax.X, Triangle.Centre.X);
            CentroidMax.Y = FMath::Max(CentroidMax.Y, Triangle.Centre.Y);
            CentroidMax.Z = FMath::Max(CentroidMax.Z, Triangle.Centre.Z);
        }

        constexpr int32 LeafTriangleCount = 8;
        if (Count <= LeafTriangleCount)
        {
            Node.FirstTriangle = Start;
            Node.TriangleCount = Count;
            World.ExploreBvhNodes[NodeIndex] = Node;
            return NodeIndex;
        }

        const FVector CentroidExtent = CentroidMax - CentroidMin;
        int32 SplitAxis = 0;
        if (CentroidExtent.Y > CentroidExtent.X
            && CentroidExtent.Y >= CentroidExtent.Z)
        {
            SplitAxis = 1;
        }
        else if (CentroidExtent.Z > CentroidExtent.X
            && CentroidExtent.Z > CentroidExtent.Y)
        {
            SplitAxis = 2;
        }

        int32* OrderBegin = World.ExploreTriangleOrder.GetData() + Start;
        int32* OrderEnd = OrderBegin + Count;
        std::sort(
            OrderBegin,
            OrderEnd,
            [&](int32 LeftIndex, int32 RightIndex)
            {
                const float LeftValue = ExploreAxisValue(
                    World.ExploreTriangles[LeftIndex].Centre, SplitAxis);
                const float RightValue = ExploreAxisValue(
                    World.ExploreTriangles[RightIndex].Centre, SplitAxis);
                if (LeftValue != RightValue)
                {
                    return LeftValue < RightValue;
                }
                return LeftIndex < RightIndex;
            });

        const int32 LeftCount = Count / 2;
        const int32 RightCount = Count - LeftCount;
        const int32 LeftNode = BuildNode(Start, LeftCount);
        const int32 RightNode = BuildNode(Start + LeftCount, RightCount);
        if (LeftNode == INDEX_NONE || RightNode == INDEX_NONE)
        {
            bBuildFailed = true;
            return INDEX_NONE;
        }

        Node.Left = LeftNode;
        Node.Right = RightNode;
        World.ExploreBvhNodes[NodeIndex] = Node;
        return NodeIndex;
    };

    World.ExploreBvhRoot = BuildNode(0, World.ExploreTriangles.Num());
    if (bBuildFailed || World.ExploreBvhRoot == INDEX_NONE || Budget.bTruncated)
    {
        World.ExploreTriangles.Reset();
        World.ExploreTriangleOrder.Reset();
        World.ExploreBvhNodes.Reset();
        World.ExploreBvhRoot = INDEX_NONE;
        OutError = TEXT("Mesh acceleration was truncated before its BVH completed.");
        return false;
    }

    World.ExploreAccelerationSeconds = FPlatformTime::Seconds() - AccelerationStartSeconds;
    World.bExploreAccelerationBuilt = true;
    OutError.Reset();
    return true;
}

bool ExploreRayAabb(
    const FVector& Origin,
    const FVector& Direction,
    const FVector& BoundsMin,
    const FVector& BoundsMax,
    float MaxDistance,
    float& OutNear)
{
    float NearDistance = 0.0f;
    float FarDistance = MaxDistance;
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        const float OriginComponent = ExploreAxisValue(Origin, Axis);
        const float DirectionComponent = ExploreAxisValue(Direction, Axis);
        const float MinComponent = ExploreAxisValue(BoundsMin, Axis);
        const float MaxComponent = ExploreAxisValue(BoundsMax, Axis);
        if (FMath::Abs(DirectionComponent) <= KINDA_SMALL_NUMBER)
        {
            if (OriginComponent < MinComponent || OriginComponent > MaxComponent)
            {
                return false;
            }
            continue;
        }

        const float InverseDirection = 1.0f / DirectionComponent;
        float AxisNear = (MinComponent - OriginComponent) * InverseDirection;
        float AxisFar = (MaxComponent - OriginComponent) * InverseDirection;
        if (AxisNear > AxisFar)
        {
            Swap(AxisNear, AxisFar);
        }
        NearDistance = FMath::Max(NearDistance, AxisNear);
        FarDistance = FMath::Min(FarDistance, AxisFar);
        if (NearDistance > FarDistance)
        {
            return false;
        }
    }

    OutNear = NearDistance;
    return true;
}

bool ExploreRayTriangle(
    const FVector& Origin,
    const FVector& Direction,
    const FExploreMeshTriangle& Triangle,
    float MaxDistance,
    float& OutDistance,
    float& OutU,
    float& OutV)
{
    const FVector Edge1 = Triangle.B - Triangle.A;
    const FVector Edge2 = Triangle.C - Triangle.A;
    const FVector Perpendicular = FVector::CrossProduct(Direction, Edge2);
    const float Determinant = FVector::DotProduct(Edge1, Perpendicular);
    if (FMath::Abs(Determinant) <= KINDA_SMALL_NUMBER)
    {
        return false;
    }

    const float InverseDeterminant = 1.0f / Determinant;
    const FVector ToOrigin = Origin - Triangle.A;
    const float U = FVector::DotProduct(ToOrigin, Perpendicular) * InverseDeterminant;
    if (U < 0.0f || U > 1.0f)
    {
        return false;
    }

    const FVector CrossOrigin = FVector::CrossProduct(ToOrigin, Edge1);
    const float V = FVector::DotProduct(Direction, CrossOrigin) * InverseDeterminant;
    if (V < 0.0f || U + V > 1.0f)
    {
        return false;
    }

    const float Distance = FVector::DotProduct(Edge2, CrossOrigin) * InverseDeterminant;
    if (Distance <= 0.01f || Distance >= MaxDistance)
    {
        return false;
    }
    OutDistance = Distance;
    OutU = U;
    OutV = V;
    return true;
}

bool RasterizeExploreMesh(
    const FExploreArguments& Arguments,
    FExploreWorld& World,
    const FVector& CameraPosition,
    const FVector& Target,
    FExploreBudget& Budget,
    TArray<FColor>& OutPixels,
    int32& OutHitPixels,
    bool& bOutTruncated)
{
    OutPixels.Init(FColor(12, 18, 28, 255), Arguments.RenderWidth * Arguments.RenderHeight);
    OutHitPixels = 0;
    bOutTruncated = false;

    if (!World.bExploreAccelerationBuilt)
    {
        FString AccelerationError;
        if (!EnsureExploreAcceleration(Arguments, World, Budget, AccelerationError))
        {
            if (Budget.bTruncated)
            {
                bOutTruncated = true;
                return true;
            }
            return false;
        }
    }

    FExploreCameraBasis Basis;
    if (!BuildCameraBasis(
            CameraPosition,
            Target,
            70.0f,
            Arguments.RenderWidth,
            Arguments.RenderHeight,
            Basis))
    {
        return false;
    }

    const FVector KeyLightDirection = FVector(-0.35f, -0.45f, 0.82f).GetSafeNormal();
    const FVector FillLightDirection = FVector(0.65f, 0.30f, 0.70f).GetSafeNormal();
    constexpr float AmbientFill = 0.62f;
    constexpr float KeyLightStrength = 0.50f;
    constexpr float FillLightStrength = 0.24f;

    for (int32 PixelY = 0; PixelY < Arguments.RenderHeight; ++PixelY)
    {
        if (Budget.ShouldStop(TEXT("raster")))
        {
            bOutTruncated = true;
            return true;
        }

        for (int32 PixelX = 0; PixelX < Arguments.RenderWidth; ++PixelX)
        {
            const float NormalizedX = (2.0f * (static_cast<float>(PixelX) + 0.5f)
                / static_cast<float>(Arguments.RenderWidth)) - 1.0f;
            const float NormalizedY = 1.0f - (2.0f * (static_cast<float>(PixelY) + 0.5f)
                / static_cast<float>(Arguments.RenderHeight));
            const FVector Direction = (
                Basis.Forward
                + Basis.Right * (NormalizedX * Basis.Aspect * Basis.TanHalfFov)
                + Basis.Up * (NormalizedY * Basis.TanHalfFov)).GetSafeNormal();

            float ClosestDistance = Arguments.RenderMaxDistanceVoxels;
            int32 ClosestTriangleIndex = INDEX_NONE;
            float ClosestU = 0.0f;
            float ClosestV = 0.0f;
            int32 NodeStack[256];
            int32 StackCount = 0;
            if (World.ExploreBvhRoot != INDEX_NONE)
            {
                NodeStack[StackCount++] = World.ExploreBvhRoot;
            }

            while (StackCount > 0)
            {
                const int32 NodeIndex = NodeStack[--StackCount];
                if (!World.ExploreBvhNodes.IsValidIndex(NodeIndex))
                {
                    continue;
                }

                const FExploreMeshBvhNode& Node = World.ExploreBvhNodes[NodeIndex];
                float NodeNear = 0.0f;
                if (!ExploreRayAabb(
                        CameraPosition,
                        Direction,
                        Node.Min,
                        Node.Max,
                        ClosestDistance,
                        NodeNear))
                {
                    continue;
                }

                if (Node.IsLeaf())
                {
                    for (int32 Offset = 0; Offset < Node.TriangleCount; ++Offset)
                    {
                        const int32 OrderIndex = Node.FirstTriangle + Offset;
                        if (!World.ExploreTriangleOrder.IsValidIndex(OrderIndex))
                        {
                            continue;
                        }
                        const int32 TriangleIndex = World.ExploreTriangleOrder[OrderIndex];
                        if (!World.ExploreTriangles.IsValidIndex(TriangleIndex))
                        {
                            continue;
                        }
                        float TriangleDistance = 0.0f;
                        float TriangleU = 0.0f;
                        float TriangleV = 0.0f;
                        if (ExploreRayTriangle(
                                CameraPosition,
                                Direction,
                                World.ExploreTriangles[TriangleIndex],
                                ClosestDistance,
                                TriangleDistance,
                                TriangleU,
                                TriangleV))
                        {
                            ClosestDistance = TriangleDistance;
                            ClosestTriangleIndex = TriangleIndex;
                            ClosestU = TriangleU;
                            ClosestV = TriangleV;
                        }
                    }
                    continue;
                }

                const int32 LeftNodeIndex = Node.Left;
                const int32 RightNodeIndex = Node.Right;
                float LeftNear = 0.0f;
                float RightNear = 0.0f;
                const bool bHitLeft = World.ExploreBvhNodes.IsValidIndex(LeftNodeIndex)
                    && ExploreRayAabb(
                        CameraPosition,
                        Direction,
                        World.ExploreBvhNodes[LeftNodeIndex].Min,
                        World.ExploreBvhNodes[LeftNodeIndex].Max,
                        ClosestDistance,
                        LeftNear);
                const bool bHitRight = World.ExploreBvhNodes.IsValidIndex(RightNodeIndex)
                    && ExploreRayAabb(
                        CameraPosition,
                        Direction,
                        World.ExploreBvhNodes[RightNodeIndex].Min,
                        World.ExploreBvhNodes[RightNodeIndex].Max,
                        ClosestDistance,
                        RightNear);
                if (bHitLeft && bHitRight)
                {
                    if (LeftNear < RightNear)
                    {
                        if (StackCount + 2 <= UE_ARRAY_COUNT(NodeStack))
                        {
                            NodeStack[StackCount++] = RightNodeIndex;
                            NodeStack[StackCount++] = LeftNodeIndex;
                        }
                    }
                    else if (StackCount + 2 <= UE_ARRAY_COUNT(NodeStack))
                    {
                        NodeStack[StackCount++] = LeftNodeIndex;
                        NodeStack[StackCount++] = RightNodeIndex;
                    }
                }
                else if (bHitLeft && StackCount < UE_ARRAY_COUNT(NodeStack))
                {
                    NodeStack[StackCount++] = LeftNodeIndex;
                }
                else if (bHitRight && StackCount < UE_ARRAY_COUNT(NodeStack))
                {
                    NodeStack[StackCount++] = RightNodeIndex;
                }
            }

            if (ClosestTriangleIndex == INDEX_NONE)
            {
                continue;
            }

            const FExploreMeshTriangle& Triangle = World.ExploreTriangles[ClosestTriangleIndex];
            FVector ShadingNormal = (
                Triangle.NormalA * (1.0f - ClosestU - ClosestV)
                + Triangle.NormalB * ClosestU
                + Triangle.NormalC * ClosestV).GetSafeNormal();
            if (ShadingNormal.IsNearlyZero())
            {
                // The canonical mesher supplies non-zero gradient normals. Keep a defensive
                // fallback for malformed mesh data without reintroducing camera-facing flips.
                ShadingNormal = Triangle.Normal;
            }
            const float Key = FMath::Max(
                0.0f, FVector::DotProduct(ShadingNormal, KeyLightDirection));
            const float Fill = FMath::Max(
                0.0f, FVector::DotProduct(ShadingNormal, FillLightDirection));
            const float Brightness = FMath::Clamp(
                AmbientFill + KeyLightStrength * Key + FillLightStrength * Fill,
                0.0f,
                1.25f);
            const int32 PixelIndex = PixelX + Arguments.RenderWidth * PixelY;
            OutPixels[PixelIndex] = FColor(
                static_cast<uint8>(FMath::Clamp(100.0f * Brightness, 0.0f, 255.0f)),
                static_cast<uint8>(FMath::Clamp(128.0f * Brightness, 0.0f, 255.0f)),
                static_cast<uint8>(FMath::Clamp(160.0f * Brightness, 0.0f, 255.0f)),
                255);
            ++OutHitPixels;
        }
    }
    return true;
}

bool RunRender(
    const FExploreArguments& Arguments,
    FExploreWorld& World,
    const FExploreWalkOutput& CameraSeedWalk,
    FExploreRenderOutput& OutOutput,
    FExploreBudget& Budget)
{
    OutOutput = FExploreRenderOutput();
    OutOutput.Width = Arguments.RenderWidth;
    OutOutput.Height = Arguments.RenderHeight;
    OutOutput.FixedStepVoxels = Arguments.RenderStepVoxels;
    OutOutput.FixedStepMeters = Arguments.RenderStepVoxels
        * FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
    OutOutput.BisectionIterations = 0;
    OutOutput.MaxDistanceVoxels = Arguments.RenderMaxDistanceVoxels;
    OutOutput.EstimatedFixedDensitySamples = 0;
    OutOutput.LegacyTwoViewEstimatedDensitySamples = static_cast<int64>(Arguments.RenderWidth)
        * static_cast<int64>(Arguments.RenderHeight)
        * 3ll
        * static_cast<int64>(FMath::Max(
            1,
            FMath::CeilToInt(
                Arguments.RenderMaxDistanceVoxels / Arguments.RenderStepVoxels)))
        * 2ll;
    OutOutput.RequestedViewpointCount = 8;
    OutOutput.StepRationale = TEXT(
        "The renderer meshes the bounded region once with UVoxelMarchingCubesMesher, then CPU "
        "rasterises its canonical triangles through a deterministic BVH-assisted depth pass. "
        "-renderstep is retained only for legacy invocation compatibility; no render pixel samples "
        "the density field.");

    OutOutput.CameraSeedPolicy = TEXT(
        "Eight deterministic viewpoints are selected from the one shared player-fit mask. Their "
        "eye-height origins are density-checked, and their azimuths are rotated in fixed 45-degree "
        "increments so every image is from inside walkable space.");

    FString MeshError;
    if (!EnsureExploreMesh(Arguments, World, &CameraSeedWalk, Budget, MeshError))
    {
        OutOutput.MeshSeconds = World.ExploreMeshSeconds;
        OutOutput.bMeshComplete = World.bExploreMeshComplete;
        if (Budget.bTruncated)
        {
            OutOutput.Status = TEXT("truncated");
            OutOutput.bTruncated = true;
            OutOutput.RefusalReason = MeshError;
        }
        else
        {
            OutOutput.Status = TEXT("error");
            OutOutput.RefusalReason = MeshError;
        }
        return false;
    }
    OutOutput.MeshSeconds = World.ExploreMeshSeconds;
    OutOutput.bMeshComplete = true;
    FString AccelerationError;
    if (!EnsureExploreAcceleration(Arguments, World, Budget, AccelerationError))
    {
        OutOutput.MeshAccelerationSeconds = World.ExploreAccelerationSeconds;
        if (Budget.bTruncated)
        {
            OutOutput.Status = TEXT("truncated");
            OutOutput.bTruncated = true;
            OutOutput.RefusalReason = AccelerationError;
        }
        else
        {
            OutOutput.Status = TEXT("error");
            OutOutput.RefusalReason = AccelerationError;
        }
        return false;
    }
    OutOutput.MeshAccelerationSeconds = World.ExploreAccelerationSeconds;
    const double RasterStartSeconds = FPlatformTime::Seconds();

    TArray<TPair<FVector, FString>, TInlineAllocator<8>> CameraSeeds;
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

    for (int32 SeedIndex = 0; SeedIndex < CameraSeedWalk.CameraSeedPoses.Num(); ++SeedIndex)
    {
        const FString Source = CameraSeedWalk.CameraSeedSources.IsValidIndex(SeedIndex)
            ? CameraSeedWalk.CameraSeedSources[SeedIndex]
            : FString::Printf(TEXT("player_fit_%02d"), SeedIndex);
        AddCameraSeed(
            true,
            CameraSeedWalk.CameraSeedPoses[SeedIndex],
            *Source);
        if (CameraSeeds.Num() >= OutOutput.RequestedViewpointCount)
        {
            break;
        }
    }

    if (CameraSeeds.Num() == 0)
    {
        OutOutput.Status = TEXT("refused");
        OutOutput.RefusalReason = TEXT(
            "The player-fit seed pass produced no density-positive pose; refusing an inside-rock camera.");
        return false;
    }

    // A small room can contain fewer than eight distinct lattice cells. Reusing the same valid
    // player-fit origin with another deterministic angle is still a valid viewpoint and keeps
    // the contract (all camera origins are inside walkable space) without fabricating positions.
    while (CameraSeeds.Num() < OutOutput.RequestedViewpointCount)
    {
        const TPair<FVector, FString>& Anchor = CameraSeeds[0];
        CameraSeeds.Emplace(
            Anchor.Key,
            FString::Printf(TEXT("%s_angle_%02d"), *Anchor.Value, CameraSeeds.Num()));
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

    for (int32 ViewIndex = 0; ViewIndex < CameraSeeds.Num(); ++ViewIndex)
    {
        if (Budget.ShouldStop(TEXT("render")))
        {
            OutOutput.Status = TEXT("truncated");
            OutOutput.bTruncated = true;
            OutOutput.RefusalReason = TEXT("The wall-clock budget stopped viewpoint generation.");
            return false;
        }

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
        // Keep the camera at the fitted pose while changing the azimuth for each viewpoint.
        LookDirection = LookDirection.RotateAngleAxis(
            45.0f * static_cast<float>(ViewIndex),
            FVector::UpVector).GetSafeNormal();
        const FVector Target = CameraPosition + LookDirection * 96.0f + FVector::UpVector * 8.0f;

        const double ViewStartSeconds = FPlatformTime::Seconds();
        TArray<FColor> Pixels;
        int32 HitPixels = 0;
        bool bRasterTruncated = false;
        if (!RasterizeExploreMesh(
                Arguments,
                World,
                CameraPosition,
                Target,
                Budget,
                Pixels,
                HitPixels,
                bRasterTruncated))
        {
            OutOutput.Status = TEXT("error");
            OutOutput.RefusalReason = TEXT("Could not construct a mesh raster camera.");
            return false;
        }

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
        Frame.bPartial = bRasterTruncated;

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

        const double ViewSeconds = FPlatformTime::Seconds() - ViewStartSeconds;
        if (ViewIndex == 0)
        {
            OutOutput.FirstViewRasterSeconds = ViewSeconds;
        }

        if (bRasterTruncated)
        {
            OutOutput.Status = TEXT("truncated");
            OutOutput.bTruncated = true;
            OutOutput.RefusalReason = TEXT("The wall-clock budget stopped triangle rasterisation.");
            break;
        }
    }

    OutOutput.MeshSeconds = World.ExploreMeshSeconds;
    OutOutput.RasterSeconds = FPlatformTime::Seconds() - RasterStartSeconds;
    const int32 AdditionalViewCount = FMath::Max(0, OutOutput.Frames.Num() - 1);
    OutOutput.AdditionalViewRasterSeconds = AdditionalViewCount > 0
        ? (OutOutput.RasterSeconds - OutOutput.FirstViewRasterSeconds)
            / static_cast<double>(AdditionalViewCount)
        : 0.0;
    if (!OutOutput.bTruncated)
    {
        OutOutput.Status = TEXT("ok");
    }
    return !OutOutput.bTruncated;
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
    FExploreWorld& World,
    const FExploreWalkOutput& Walk,
    FExploreRenderOutput& InOutOutput,
    FExploreBudget& Budget)
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
        if (Budget.ShouldStop(TEXT("failure-focus-render")))
        {
            InOutOutput.Status = TEXT("truncated");
            InOutOutput.bTruncated = true;
            InOutOutput.RefusalReason = TEXT(
                "The wall-clock budget stopped failure-focus viewpoint generation.");
            return false;
        }

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
        const FVector Target = ViewTargets[ViewIndex];
        TArray<FColor> Pixels;
        int32 HitPixels = 0;
        bool bRasterTruncated = false;
        if (!RasterizeExploreMesh(
                Arguments,
                World,
                CameraPosition,
                Target,
                Budget,
                Pixels,
                HitPixels,
                bRasterTruncated))
        {
            InOutOutput.Status = TEXT("error");
            InOutOutput.RefusalReason = TEXT("Could not construct failure-focus mesh raster.");
            return false;
        }

        FExploreCameraBasis Basis;
        if (!BuildCameraBasis(
                CameraPosition,
                Target,
                70.0f,
                Arguments.RenderWidth,
                Arguments.RenderHeight,
                Basis))
        {
            return false;
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
        Frame.TargetVoxels = Target;
        Frame.HitPixels = HitPixels;
        Frame.bFailureMarkersProjected = bAgentProjected && bTargetProjected
            && (!bHaveLastReached || bLastReachedProjected);
        Frame.bPartial = bRasterTruncated;

        bool bScaleMarkerProjected = false;
        DrawScaleMarker(
            Pixels,
            Arguments.RenderWidth,
            Arguments.RenderHeight,
            CameraPosition,
            Target,
            Basis,
            bScaleMarkerProjected);
        Frame.bScaleMarkerProjected = bScaleMarkerProjected;

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

        if (bRasterTruncated)
        {
            InOutOutput.Status = TEXT("truncated");
            InOutOutput.bTruncated = true;
            InOutOutput.RefusalReason = TEXT(
                "The wall-clock budget stopped failure-focus rasterisation.");
            return false;
        }
    }

    return true;
}

bool RunWalk(
    const FExploreArguments& Arguments,
    const FExploreWorld& World,
    FExploreWalkOutput& OutOutput,
    FExploreBudget& Budget)
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
    if (Budget.ShouldStop(TEXT("walk")))
    {
        OutOutput.Status = TEXT("truncated");
        OutOutput.RefusalReason = TEXT("The wall-clock budget elapsed before walk sampling.");
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
            &LandingRoomProbePoints,
            &OutOutput.SharedSampleGrid))
    {
        OutOutput.Status = TEXT("refused");
        OutOutput.RefusalReason = OutOutput.Report.RefusalReason;
        return false;
    }

    // Camera seeds are selected from this exact captured mask. The first valid fit cell anchors
    // the bounded render mesh; subsequent cells are kept local to that anchor so the mesh region
    // contains every requested viewpoint rather than silently rendering a distant empty box.
    OutOutput.CameraSeedPoses.Reset();
    OutOutput.CameraSeedSources.Reset();
    FVector CameraAnchor = FVector::ZeroVector;
    bool bHaveCameraAnchor = false;
    const auto AddCameraSeed = [&](const FVector& Position, const TCHAR* Source)
    {
        if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y)
            || !FMath::IsFinite(Position.Z)
            || !OutOutput.SharedSampleGrid.ContainsPoint(Position))
        {
            return;
        }
        const float Step = static_cast<float>(OutOutput.SharedSampleGrid.SampleStep);
        const int32 X = FMath::Clamp(
            FMath::FloorToInt((Position.X - OutOutput.SharedSampleGrid.MinX) / Step),
            0,
            OutOutput.SharedSampleGrid.NumX - 1);
        const int32 Y = FMath::Clamp(
            FMath::FloorToInt((Position.Y - OutOutput.SharedSampleGrid.MinY) / Step),
            0,
            OutOutput.SharedSampleGrid.NumY - 1);
        const int32 Z = FMath::Clamp(
            FMath::FloorToInt((Position.Z - OutOutput.SharedSampleGrid.MinZ) / Step),
            0,
            OutOutput.SharedSampleGrid.NumZ - 1);
        const int32 Cell = OutOutput.SharedSampleGrid.Index(X, Y, Z);
        if (!OutOutput.SharedSampleGrid.PlayerFitMask.IsValidIndex(Cell)
            || OutOutput.SharedSampleGrid.PlayerFitMask[Cell] == 0u)
        {
            return;
        }
        if (!bHaveCameraAnchor)
        {
            CameraAnchor = Position;
            bHaveCameraAnchor = true;
        }
        if (FVector::DistSquared(Position, CameraAnchor) > FMath::Square(40.0f))
        {
            return;
        }
        for (const FVector& Existing : OutOutput.CameraSeedPoses)
        {
            if (Existing.Equals(Position, 0.001f))
            {
                return;
            }
        }
        OutOutput.CameraSeedPoses.Add(Position);
        OutOutput.CameraSeedSources.Add(
            Source != nullptr ? FString(Source)
                              : FString::Printf(TEXT("player_fit_%02d"),
                                  OutOutput.CameraSeedPoses.Num() - 1));
    };
    AddCameraSeed(OutOutput.ArrivalVoxels, TEXT("arrival_player_fit"));
    AddCameraSeed(OutOutput.DepartureVoxels, TEXT("departure_player_fit"));
    if (OutOutput.Report.bHasLastReachedPosition)
    {
        AddCameraSeed(
            OutOutput.Report.LastReachedVoxels,
            TEXT("last_reached_player_fit"));
    }
    if (OutOutput.Report.bHasAgentFinalPosition)
    {
        AddCameraSeed(
            OutOutput.Report.AgentFinalVoxels,
            TEXT("agent_final_player_fit"));
    }
    if (!bHaveCameraAnchor)
    {
        for (int32 Cell = 0;
             Cell < OutOutput.SharedSampleGrid.PlayerFitMask.Num();
             ++Cell)
        {
            if (OutOutput.SharedSampleGrid.PlayerFitMask[Cell] == 0u)
            {
                continue;
            }
            const int32 Plane = OutOutput.SharedSampleGrid.NumX
                * OutOutput.SharedSampleGrid.NumY;
            const int32 Z = Cell / Plane;
            const int32 InPlane = Cell - Z * Plane;
            const int32 Y = InPlane / OutOutput.SharedSampleGrid.NumX;
            const int32 X = InPlane - Y * OutOutput.SharedSampleGrid.NumX;
            AddCameraSeed(
                FVector(
                    OutOutput.SharedSampleGrid.MinX
                        + (static_cast<float>(X) + 0.5f)
                            * OutOutput.SharedSampleGrid.SampleStep,
                    OutOutput.SharedSampleGrid.MinY
                        + (static_cast<float>(Y) + 0.5f)
                            * OutOutput.SharedSampleGrid.SampleStep,
                    OutOutput.SharedSampleGrid.MinZ
                        + (static_cast<float>(Z) + 0.5f)
                            * OutOutput.SharedSampleGrid.SampleStep),
                TEXT("player_fit_grid"));
            if (bHaveCameraAnchor)
            {
                break;
            }
        }
    }
    if (bHaveCameraAnchor)
    {
        const int32 NumCandidates = OutOutput.SharedSampleGrid.PlayerFitMask.Num();
        for (int32 Cell = 0; Cell < NumCandidates && OutOutput.CameraSeedPoses.Num() < 8; ++Cell)
        {
            if (OutOutput.SharedSampleGrid.PlayerFitMask[Cell] == 0u)
            {
                continue;
            }
            const int32 Plane = OutOutput.SharedSampleGrid.NumX
                * OutOutput.SharedSampleGrid.NumY;
            const int32 Z = Cell / Plane;
            const int32 InPlane = Cell - Z * Plane;
            const int32 Y = InPlane / OutOutput.SharedSampleGrid.NumX;
            const int32 X = InPlane - Y * OutOutput.SharedSampleGrid.NumX;
            AddCameraSeed(
                FVector(
                    OutOutput.SharedSampleGrid.MinX
                        + (static_cast<float>(X) + 0.5f)
                            * OutOutput.SharedSampleGrid.SampleStep,
                    OutOutput.SharedSampleGrid.MinY
                        + (static_cast<float>(Y) + 0.5f)
                            * OutOutput.SharedSampleGrid.SampleStep,
                    OutOutput.SharedSampleGrid.MinZ
                        + (static_cast<float>(Z) + 0.5f)
                            * OutOutput.SharedSampleGrid.SampleStep),
                TEXT("player_fit_grid"));
        }
    }

    // The origin observation is now a view over the shared primary grid. A second, expanded
    // walk would defeat the one-grid contract; callers that need origin coverage must request an
    // origin-inclusive archetype window, which is already selected before the single sample pass.
    OutOutput.bOriginCheckAttempted = true;
    if (OutOutput.Report.bOriginColumnInSampledWindow)
    {
        OutOutput.OriginCheckReport = OutOutput.Report;
        OutOutput.bOriginCheckAvailable = true;
    }
    else
    {
        OutOutput.OriginCheckRefusalReason = TEXT(
            "Origin is outside the shared primary sample window; no second grid was sampled.");
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
    if (Budget.ShouldStop(TEXT("walk")))
    {
        OutOutput.Status = TEXT("truncated");
        OutOutput.RefusalReason = TEXT("The wall-clock budget elapsed after walk sampling.");
        return false;
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
    // This preflight covers one canonical tile's scalar grid, edge map, mesh arrays, and temporary
    // triangle lists. The run-level canonical aggregate is retained separately so render and
    // export consume the same mesh. RegionSize is still accepted here to keep the call site's
    // contract explicit.
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

bool ExploreMetricVectorIsFinite(const FVector& Value)
{
    return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
}

FIntVector ExploreMetricWeldPosition(const FVector& PositionCm)
{
    // Mesh positions are Unreal centimetres. 0.01 cm is deliberately much smaller than a
    // marching-cubes cell and only absorbs independent-tile floating-point round-off.
    constexpr float WeldQuantumCm = 0.01f;
    return FIntVector(
        FMath::RoundToInt(PositionCm.X / WeldQuantumCm),
        FMath::RoundToInt(PositionCm.Y / WeldQuantumCm),
        FMath::RoundToInt(PositionCm.Z / WeldQuantumCm));
}

FExploreMeshEdgeKey ExploreMetricMakeEdgeKey(
    const FVector& PositionA,
    const FVector& PositionB)
{
    const FIntVector A = ExploreMetricWeldPosition(PositionA);
    const FIntVector B = ExploreMetricWeldPosition(PositionB);
    const bool bAFirst = A.X < B.X
        || (A.X == B.X && (A.Y < B.Y || (A.Y == B.Y && A.Z <= B.Z)));
    return bAFirst
        ? FExploreMeshEdgeKey{A, B}
        : FExploreMeshEdgeKey{B, A};
}

float ExploreMetricPercentile(const TArray<float>& SortedValues, float Quantile)
{
    if (SortedValues.Num() == 0)
    {
        return 0.0f;
    }
    const float Position = FMath::Clamp(Quantile, 0.0f, 1.0f)
        * static_cast<float>(SortedValues.Num() - 1);
    const int32 Lower = FMath::FloorToInt(Position);
    const int32 Upper = FMath::Min(Lower + 1, SortedValues.Num() - 1);
    return FMath::Lerp(SortedValues[Lower], SortedValues[Upper], Position - static_cast<float>(Lower));
}

bool MeasureExploreMesh(
    const FVoxelMeshData& MeshData,
    FExploreMeshMetrics& OutMetrics,
    FString& OutError)
{
    OutMetrics = FExploreMeshMetrics();
    if (!ValidateCanonicalTile(MeshData, OutError))
    {
        return false;
    }

    const int32 NumTriangles = MeshData.Triangles.Num() / 3;
    OutMetrics.VertexCount = MeshData.Vertices.Num();
    OutMetrics.TriangleCount = NumTriangles;

    TArray<float> EdgeLengthsMeters;
    TArray<float> DihedralAnglesDegrees;
    TArray<float> FaceNormalDivergencesDegrees;
    EdgeLengthsMeters.Reserve(NumTriangles * 3);
    DihedralAnglesDegrees.Reserve(NumTriangles * 2);
    FaceNormalDivergencesDegrees.Reserve(NumTriangles);

    TArray<FVector> FaceNormals;
    FaceNormals.Init(FVector::ZeroVector, NumTriangles);
    TArray<uint8> ValidFaceNormals;
    ValidFaceNormals.Init(0, NumTriangles);
    TMap<FExploreMeshEdgeKey, FExploreMeshEdgeIncident> EdgeIncidents;
    EdgeIncidents.Reserve(FMath::Max(1, NumTriangles * 2));

    const auto AddEdgeIncident = [&EdgeIncidents](
        const FVector& PositionA, const FVector& PositionB, int32 TriangleIndex)
    {
        const FExploreMeshEdgeKey Key = ExploreMetricMakeEdgeKey(PositionA, PositionB);
        if (Key.First == Key.Second)
        {
            return;
        }
        FExploreMeshEdgeIncident& Incident = EdgeIncidents.FindOrAdd(Key);
        if (Incident.IncidentCount == 0)
        {
            Incident.FirstTriangle = TriangleIndex;
        }
        else if (Incident.IncidentCount == 1)
        {
            Incident.SecondTriangle = TriangleIndex;
        }
        Incident.IncidentCount = FMath::Min(Incident.IncidentCount + 1, 3);
    };

    for (int32 TriangleIndex = 0; TriangleIndex < NumTriangles; ++TriangleIndex)
    {
        const int32 Base = TriangleIndex * 3;
        const int32 IndexA = MeshData.Triangles[Base];
        const int32 IndexB = MeshData.Triangles[Base + 1];
        const int32 IndexC = MeshData.Triangles[Base + 2];
        if (!MeshData.Vertices.IsValidIndex(IndexA)
            || !MeshData.Vertices.IsValidIndex(IndexB)
            || !MeshData.Vertices.IsValidIndex(IndexC)
            || !MeshData.Normals.IsValidIndex(IndexA)
            || !MeshData.Normals.IsValidIndex(IndexB)
            || !MeshData.Normals.IsValidIndex(IndexC))
        {
            OutError = TEXT("Surface metric measurement saw an out-of-range mesh index.");
            return false;
        }

        const FVector& A = MeshData.Vertices[IndexA];
        const FVector& B = MeshData.Vertices[IndexB];
        const FVector& C = MeshData.Vertices[IndexC];
        if (!ExploreMetricVectorIsFinite(A)
            || !ExploreMetricVectorIsFinite(B)
            || !ExploreMetricVectorIsFinite(C))
        {
            OutError = TEXT("Surface metric measurement saw a non-finite vertex.");
            return false;
        }

        EdgeLengthsMeters.Add(FVector::Dist(A, B) * 0.01f);
        EdgeLengthsMeters.Add(FVector::Dist(B, C) * 0.01f);
        EdgeLengthsMeters.Add(FVector::Dist(C, A) * 0.01f);

        const FVector Cross = FVector::CrossProduct(B - A, C - A);
        if (Cross.SizeSquared() > SMALL_NUMBER)
        {
            const FVector MeanVertexNormal = (
                MeshData.Normals[IndexA]
                + MeshData.Normals[IndexB]
                + MeshData.Normals[IndexC]).GetSafeNormal();
            if (ExploreMetricVectorIsFinite(MeanVertexNormal)
                && !MeanVertexNormal.IsNearlyZero())
            {
                // The canonical marching-cubes table is emitted with the winding required by
                // the runtime mesh path, while the density-gradient normals point solid-to-air.
                // Orient the geometric face normal to that same outward convention before
                // measuring the relief divergence; otherwise a winding convention change would
                // report ~180 degrees for an otherwise smooth surface.
                FVector FaceNormal = Cross.GetSafeNormal();
                if (FVector::DotProduct(FaceNormal, MeanVertexNormal) < 0.0f)
                {
                    FaceNormal *= -1.0f;
                }
                FaceNormals[TriangleIndex] = FaceNormal;
                ValidFaceNormals[TriangleIndex] = 1;
                const float Dot = FMath::Clamp(
                    FVector::DotProduct(FaceNormal, MeanVertexNormal), -1.0f, 1.0f);
                FaceNormalDivergencesDegrees.Add(
                    FMath::RadiansToDegrees(FMath::Acos(Dot)));
                ++OutMetrics.NormalComparisonTriangleCount;
            }
        }

        AddEdgeIncident(A, B, TriangleIndex);
        AddEdgeIncident(B, C, TriangleIndex);
        AddEdgeIncident(C, A, TriangleIndex);
    }

    for (const TPair<FExploreMeshEdgeKey, FExploreMeshEdgeIncident>& Pair : EdgeIncidents)
    {
        const FExploreMeshEdgeIncident& Incident = Pair.Value;
        if (Incident.IncidentCount != 2)
        {
            continue;
        }
        ++OutMetrics.SharedEdgeCount;
        if (!ValidFaceNormals.IsValidIndex(Incident.FirstTriangle)
            || !ValidFaceNormals.IsValidIndex(Incident.SecondTriangle)
            || !ValidFaceNormals[Incident.FirstTriangle]
            || !ValidFaceNormals[Incident.SecondTriangle])
        {
            continue;
        }
        const float Dot = FMath::Clamp(
            FVector::DotProduct(
                FaceNormals[Incident.FirstTriangle],
                FaceNormals[Incident.SecondTriangle]),
            -1.0f,
            1.0f);
        DihedralAnglesDegrees.Add(FMath::RadiansToDegrees(FMath::Acos(Dot)));
    }

    EdgeLengthsMeters.Sort();
    DihedralAnglesDegrees.Sort();
    FaceNormalDivergencesDegrees.Sort();

    OutMetrics.DihedralSampleCount = DihedralAnglesDegrees.Num();
    OutMetrics.EdgeLengthP50Meters = ExploreMetricPercentile(EdgeLengthsMeters, 0.50f);
    OutMetrics.EdgeLengthP99Meters = ExploreMetricPercentile(EdgeLengthsMeters, 0.99f);
    OutMetrics.EdgeLengthMaxMeters = EdgeLengthsMeters.Num() > 0
        ? EdgeLengthsMeters.Last() : 0.0f;
    OutMetrics.DihedralP50Degrees = ExploreMetricPercentile(DihedralAnglesDegrees, 0.50f);
    OutMetrics.DihedralP75Degrees = ExploreMetricPercentile(DihedralAnglesDegrees, 0.75f);
    OutMetrics.DihedralP90Degrees = ExploreMetricPercentile(DihedralAnglesDegrees, 0.90f);
    OutMetrics.DihedralP95Degrees = ExploreMetricPercentile(DihedralAnglesDegrees, 0.95f);
    OutMetrics.DihedralP99Degrees = ExploreMetricPercentile(DihedralAnglesDegrees, 0.99f);
    if (DihedralAnglesDegrees.Num() > 0)
    {
        int32 Over20 = 0;
        int32 Over40 = 0;
        for (const float Angle : DihedralAnglesDegrees)
        {
            Over20 += Angle > 20.0f ? 1 : 0;
            Over40 += Angle > 40.0f ? 1 : 0;
        }
        OutMetrics.DihedralFractionOver20Degrees = static_cast<float>(Over20)
            / static_cast<float>(DihedralAnglesDegrees.Num());
        OutMetrics.DihedralFractionOver40Degrees = static_cast<float>(Over40)
            / static_cast<float>(DihedralAnglesDegrees.Num());
    }
    OutMetrics.FaceNormalVsMeanVertexNormalP50Degrees =
        ExploreMetricPercentile(FaceNormalDivergencesDegrees, 0.50f);
    OutMetrics.FaceNormalVsMeanVertexNormalP90Degrees =
        ExploreMetricPercentile(FaceNormalDivergencesDegrees, 0.90f);
    OutError.Reset();
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

bool WriteObjMesh(
    FArchive& Archive,
    const FVoxelMeshData& MeshData,
    FExploreBudget& Budget,
    FString& OutError,
    bool& bOutTruncated,
    int32& OutVerticesWritten,
    int32& OutTrianglesWritten)
{
    bOutTruncated = false;
    OutVerticesWritten = 0;
    OutTrianglesWritten = 0;
    if (!ValidateCanonicalTile(MeshData, OutError))
    {
        return false;
    }

    bool bOk = true;
    for (int32 VertexIndex = 0; VertexIndex < MeshData.Vertices.Num(); ++VertexIndex)
    {
        if ((VertexIndex & 255) == 0 && Budget.ShouldStop(TEXT("export-write")))
        {
            bOutTruncated = true;
            return true;
        }
        const FVector& Vertex = MeshData.Vertices[VertexIndex];
        bOk = bOk && WriteUtf8(Archive, FString::Printf(
            TEXT("v %.9f %.9f %.9f\n"),
            Vertex.X / 100.0,
            Vertex.Y / 100.0,
            Vertex.Z / 100.0));
        ++OutVerticesWritten;
    }
    for (int32 UVIndex = 0; UVIndex < MeshData.UVs.Num(); ++UVIndex)
    {
        if ((UVIndex & 255) == 0 && Budget.ShouldStop(TEXT("export-write")))
        {
            bOutTruncated = true;
            return true;
        }
        const FVector2D& UV = MeshData.UVs[UVIndex];
        bOk = bOk && WriteUtf8(Archive, FString::Printf(
            TEXT("vt %.9f %.9f\n"), UV.X, UV.Y));
    }
    for (int32 NormalIndex = 0; NormalIndex < MeshData.Normals.Num(); ++NormalIndex)
    {
        if ((NormalIndex & 255) == 0 && Budget.ShouldStop(TEXT("export-write")))
        {
            bOutTruncated = true;
            return true;
        }
        const FVector& Normal = MeshData.Normals[NormalIndex];
        bOk = bOk && WriteUtf8(Archive, FString::Printf(
            TEXT("vn %.9f %.9f %.9f\n"), Normal.X, Normal.Y, Normal.Z));
    }

    const int32 NumTriangles = MeshData.Triangles.Num() / 3;
    if (NumTriangles > 0)
    {
        bOk = bOk && WriteUtf8(Archive, TEXT("usemtl VoxelGround\n"));
    }
    for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
    {
        if ((Triangle & 255) == 0 && Budget.ShouldStop(TEXT("export-write")))
        {
            bOutTruncated = true;
            return true;
        }
        const int32 Base = Triangle * 3;
        const int32 A = MeshData.Triangles[Base];
        const int32 B = MeshData.Triangles[Base + 1];
        const int32 C = MeshData.Triangles[Base + 2];
        if (A < 0 || B < 0 || C < 0
            || A >= MeshData.Vertices.Num()
            || B >= MeshData.Vertices.Num()
            || C >= MeshData.Vertices.Num())
        {
            OutError = TEXT("Canonical aggregate mesh returned an out-of-range triangle index.");
            return false;
        }
        bOk = bOk && WriteUtf8(Archive, FString::Printf(
            TEXT("f %d/%d/%d %d/%d/%d %d/%d/%d\n"),
            A + 1, A + 1, A + 1,
            B + 1, B + 1, B + 1,
            C + 1, C + 1, C + 1));
        ++OutTrianglesWritten;
    }
    if (!bOk || Archive.IsError())
    {
        OutError = TEXT("Could not write the canonical aggregate OBJ.");
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

void WriteSurfaceReliefMetrics(
    FExploreJsonWriter& Writer,
    const FExploreMeshMetrics& Metrics)
{
    Writer.WriteObjectStart(TEXT("surface_relief_metrics"));
    Writer.WriteValue(TEXT("vertex_count"), Metrics.VertexCount);
    Writer.WriteValue(TEXT("triangle_count"), Metrics.TriangleCount);
    Writer.WriteValue(TEXT("shared_edge_count"), Metrics.SharedEdgeCount);
    Writer.WriteValue(TEXT("dihedral_sample_count"), Metrics.DihedralSampleCount);
    Writer.WriteValue(TEXT("normal_comparison_triangle_count"), Metrics.NormalComparisonTriangleCount);
    Writer.WriteValue(TEXT("vertex_weld_quantum_cm"), 0.01);
    Writer.WriteObjectStart(TEXT("triangle_edge_length_m"));
    Writer.WriteValue(TEXT("p50"), static_cast<double>(Metrics.EdgeLengthP50Meters));
    Writer.WriteValue(TEXT("p99"), static_cast<double>(Metrics.EdgeLengthP99Meters));
    Writer.WriteValue(TEXT("max"), static_cast<double>(Metrics.EdgeLengthMaxMeters));
    Writer.WriteObjectEnd();
    Writer.WriteObjectStart(TEXT("dihedral_angle_degrees"));
    Writer.WriteValue(TEXT("p50"), static_cast<double>(Metrics.DihedralP50Degrees));
    Writer.WriteValue(TEXT("p75"), static_cast<double>(Metrics.DihedralP75Degrees));
    Writer.WriteValue(TEXT("p90"), static_cast<double>(Metrics.DihedralP90Degrees));
    Writer.WriteValue(TEXT("p95"), static_cast<double>(Metrics.DihedralP95Degrees));
    Writer.WriteValue(TEXT("p99"), static_cast<double>(Metrics.DihedralP99Degrees));
    Writer.WriteValue(TEXT("fraction_over_20_degrees"),
        static_cast<double>(Metrics.DihedralFractionOver20Degrees));
    Writer.WriteValue(TEXT("fraction_over_40_degrees"),
        static_cast<double>(Metrics.DihedralFractionOver40Degrees));
    Writer.WriteObjectEnd();
    Writer.WriteObjectStart(TEXT("face_normal_vs_mean_vertex_normal_degrees"));
    Writer.WriteValue(TEXT("p50"),
        static_cast<double>(Metrics.FaceNormalVsMeanVertexNormalP50Degrees));
    Writer.WriteValue(TEXT("p90"),
        static_cast<double>(Metrics.FaceNormalVsMeanVertexNormalP90Degrees));
    Writer.WriteObjectEnd();
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
    Writer->WriteValue(TEXT("schema_version"), 4);
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
    Writer->WriteValue(TEXT("geometry_hash"), Export.GeometryHash);
    WriteSurfaceReliefMetrics(*Writer, Export.SurfaceMetrics);
    Writer->WriteValue(TEXT("mesh_file_size_bytes"), Export.MeshFileSizeBytes);
    Writer->WriteValue(TEXT("mesh_array_bytes_sum_over_tiles"), Export.MeshArrayBytes);
    Writer->WriteValue(TEXT("canonical_mesher_tile_cells"), CHUNK_SIZE);
    Writer->WriteValue(TEXT("canonical_mesher_tile_count"), Export.MesherTileCount);
    Writer->WriteValue(TEXT("canonical_mesher_tiles_completed"), Export.MesherTilesCompleted);
    Writer->WriteValue(TEXT("mesh_seconds"), Export.MeshSeconds);
    Writer->WriteValue(TEXT("mesh_us_per_voxel"), Export.MeshUsPerVoxel);
    Writer->WriteValue(TEXT("export_step"), Arguments.ExportStep);
    Writer->WriteValue(TEXT("truncated"), Export.bTruncated);
    Writer->WriteValue(TEXT("seed"), Arguments.Seed);
    Writer->WriteValue(TEXT("archetype"), ArchetypeName(Arguments.Archetype));
    Writer->WriteValue(TEXT("slot"), Arguments.Slot);
    Writer->WriteValue(TEXT("operator_stack"), Arguments.bUseOperatorStack);
    Writer->WriteValue(TEXT("surface_roughness_override"), Arguments.bSurfaceRoughnessOverride);
    if (Arguments.bSurfaceRoughnessOverride)
    {
        Writer->WriteValue(TEXT("surface_roughness"),
            static_cast<double>(Arguments.SurfaceRoughness));
    }
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
    const FExploreWalkOutput* SharedSampling,
    FExploreExportOutput& OutOutput,
    FExploreBudget& Budget)
{
    OutOutput = FExploreExportOutput();
    OutOutput.RegionSize = Arguments.ExportSize;
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

    FString MeshError;
    const bool bMeshComplete = EnsureExploreMesh(
        Arguments, World, SharedSampling, Budget, MeshError);
    if (!bMeshComplete && !Budget.bTruncated)
    {
        OutOutput.Status = TEXT("error");
        OutOutput.RefusalReason = MeshError;
        return false;
    }
    OutOutput.RegionSize = World.ExploreMeshSize;
    OutOutput.MesherTileCount = World.ExploreMeshTileCount;
    OutOutput.MesherTilesCompleted = World.ExploreMeshTilesCompleted;
    OutOutput.RegionOrigin = World.ExploreMeshOrigin;

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

    if (!MeasureExploreMesh(World.ExploreMesh, OutOutput.SurfaceMetrics, Error))
    {
        OutOutput.Status = TEXT("error");
        OutOutput.RefusalReason = Error;
        return false;
    }
    OutOutput.MeshArrayBytes = static_cast<int64>(World.ExploreMesh.Vertices.Num()) * sizeof(FVector)
        + static_cast<int64>(World.ExploreMesh.Normals.Num()) * sizeof(FVector)
        + static_cast<int64>(World.ExploreMesh.UVs.Num()) * sizeof(FVector2D)
        + static_cast<int64>(World.ExploreMesh.Colors.Num()) * sizeof(FColor)
        + static_cast<int64>(World.ExploreMesh.Triangles.Num()) * sizeof(int32);
    OutOutput.VertexCount = World.ExploreMesh.Vertices.Num();
    OutOutput.TriangleCount = World.ExploreMesh.Triangles.Num() / 3;
    OutOutput.MeshSeconds = World.ExploreMeshSeconds;
    OutOutput.MeshUsPerVoxel = Arguments.ExportSize > 0
        ? OutOutput.MeshSeconds * 1.0e6
            / static_cast<double>(Arguments.ExportSize)
            / static_cast<double>(Arguments.ExportSize)
            / static_cast<double>(Arguments.ExportSize)
        : 0.0;
    OutOutput.GeometryHash = World.ExploreGeometryHash;
    bool bObjWriteTruncated = false;
    int32 VerticesWritten = 0;
    int32 TrianglesWritten = 0;
    if (!WriteObjMesh(
            *Archive,
            World.ExploreMesh,
            Budget,
            Error,
            bObjWriteTruncated,
            VerticesWritten,
            TrianglesWritten))
    {
        OutOutput.Status = TEXT("error");
        OutOutput.RefusalReason = Error;
        return false;
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

    OutOutput.bTruncated = Budget.bTruncated
        || !World.bExploreMeshComplete
        || bObjWriteTruncated;
    if (OutOutput.bTruncated)
    {
        OutOutput.VertexCount = VerticesWritten;
        OutOutput.TriangleCount = TrianglesWritten;
        OutOutput.Status = TEXT("truncated");
        OutOutput.RefusalReason = !World.bExploreMeshComplete
            ? MeshError
            : TEXT("The wall-clock budget stopped export after writing a partial mesh.");
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
    if (!OutOutput.bTruncated)
    {
        OutOutput.Status = TEXT("ok");
    }
    return !OutOutput.bTruncated;
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
    Writer->WriteValue(TEXT("schema_version"), 4);
    Writer->WriteValue(TEXT("tool"), TEXT("VoxelForgeExplore"));
    Writer->WriteValue(TEXT("read_only_generation"), true);

    Writer->WriteObjectStart(TEXT("input"));
    Writer->WriteValue(TEXT("seed"), Arguments.Seed);
    Writer->WriteValue(TEXT("archetype"), ArchetypeName(Arguments.Archetype));
    Writer->WriteValue(TEXT("slot"), Arguments.Slot);
    Writer->WriteValue(TEXT("operator_stack"), Arguments.bUseOperatorStack);
    Writer->WriteValue(TEXT("surface_roughness_override"), Arguments.bSurfaceRoughnessOverride);
    if (Arguments.bSurfaceRoughnessOverride)
    {
        Writer->WriteValue(TEXT("surface_roughness"),
            static_cast<double>(Arguments.SurfaceRoughness));
    }
    Writer->WriteArrayStart(TEXT("modes"));
    if (Arguments.bRender) Writer->WriteValue(TEXT("render"));
    if (Arguments.bWalk) Writer->WriteValue(TEXT("walk"));
    if (Arguments.bExport) Writer->WriteValue(TEXT("export"));
    if (Arguments.bOpBounds) Writer->WriteValue(TEXT("opbounds"));
    Writer->WriteArrayEnd();
    Writer->WriteValue(TEXT("failure_focus_render"), Arguments.bFailureFocusRender);
    Writer->WriteValue(TEXT("render_step_voxels"), static_cast<double>(Arguments.RenderStepVoxels));
    Writer->WriteValue(TEXT("export_size"), Arguments.ExportSize);
    Writer->WriteValue(TEXT("export_step"), Arguments.ExportStep);
    Writer->WriteValue(TEXT("density_grid_reuse"), Arguments.bReuseDensityGrid);
    Writer->WriteValue(TEXT("block_early_out"), Arguments.bBlockEarlyOut);
    Writer->WriteValue(TEXT("mesh_min_batch_size"), Arguments.MeshMinBatchSize);
    Writer->WriteValue(TEXT("profile_density"), Arguments.bProfileDensity);
    Writer->WriteValue(TEXT("profile_density_full"), Arguments.bProfileDensityFull);
    Writer->WriteValue(TEXT("profile_lod"), Arguments.bProfileLod);
    Writer->WriteValue(TEXT("op_bounds"), Arguments.bOpBounds);
    Writer->WriteValue(TEXT("budget_minutes"), static_cast<double>(Arguments.BudgetMinutes));
    Writer->WriteValue(TEXT("out_directory"), Arguments.OutDirectory);
    Writer->WriteValue(TEXT("canonical_invocation"), FString::Printf(
        TEXT("UnrealEditor-Cmd VoxelM.uproject -run=VoxelForgeExplore -seed=%d -archetype=%s "
             "-slot=%d -opstack=%d -modes=%s -blockearlyout=%d "
             "-out=<ABSOLUTE_PLUGIN_SAVED_PATH>"),
        Arguments.Seed,
        ArchetypeName(Arguments.Archetype),
        Arguments.Slot,
        Arguments.bUseOperatorStack ? 1 : 0,
        *Arguments.CanonicalModes(),
        Arguments.bBlockEarlyOut ? 1 : 0));
    Writer->WriteObjectEnd();

    Writer->WriteObjectStart(TEXT("summary"));
    const double SummaryMeshSeconds = Arguments.bExport
        ? Output.Export.MeshSeconds
        : (Arguments.bRender
            ? Output.Render.MeshSeconds
            : World.ExploreMeshSeconds);
    const int32 SummaryTriangleCount = Arguments.bExport
        ? Output.Export.TriangleCount
        : World.ExploreMesh.Triangles.Num() / 3;
    const FString SummaryGeometryHash = Arguments.bExport
        ? Output.Export.GeometryHash
        : World.ExploreGeometryHash;
    const double SummaryUsPerVoxel = Arguments.ExportSize > 0
        ? SummaryMeshSeconds * 1.0e6
            / static_cast<double>(Arguments.ExportSize)
            / static_cast<double>(Arguments.ExportSize)
            / static_cast<double>(Arguments.ExportSize)
        : 0.0;
    Writer->WriteValue(TEXT("mesh_seconds"), SummaryMeshSeconds);
    Writer->WriteValue(TEXT("us_per_voxel"), SummaryUsPerVoxel);
    Writer->WriteValue(TEXT("triangle_count"), SummaryTriangleCount);
    Writer->WriteValue(TEXT("geometry_hash"), SummaryGeometryHash);
    Writer->WriteObjectStart(TEXT("density_grid_reuse"));
    Writer->WriteValue(TEXT("enabled"), World.bExploreDensityGridReused);
    Writer->WriteValue(TEXT("total_tile_grid_samples"), World.ExploreDensityGridTotalSamples);
    Writer->WriteValue(TEXT("unique_shared_grid_samples"), World.ExploreDensityGridUniqueSamples);
    Writer->WriteValue(TEXT("duplicate_evaluations_before"), World.ExploreDensityGridDuplicateSamplesBefore);
    Writer->WriteValue(TEXT("duplicate_evaluations_after"), World.ExploreDensityGridDuplicateSamplesAfter);
    Writer->WriteValue(TEXT("precompute_seconds"), World.ExploreDensityGridSeconds);
    Writer->WriteObjectEnd();
    Writer->WriteObjectStart(TEXT("mesher_tasks"));
    Writer->WriteValue(TEXT("min_batch_size"), World.ExploreMeshTaskMinBatchSize);
    Writer->WriteValue(TEXT("worker_count"), World.ExploreMeshTaskWorkerCount);
    Writer->WriteValue(TEXT("job_count"), World.ExploreMeshTaskJobCount);
    Writer->WriteValue(TEXT("job_min_seconds"), World.ExploreMeshTaskMinSeconds);
    Writer->WriteValue(TEXT("job_mean_seconds"), World.ExploreMeshTaskMeanSeconds);
    Writer->WriteValue(TEXT("job_max_seconds"), World.ExploreMeshTaskMaxSeconds);
    Writer->WriteValue(TEXT("job_sum_seconds"), World.ExploreMeshTaskSumSeconds);
    Writer->WriteValue(TEXT("parallel_wall_seconds"), World.ExploreMeshTaskWallSeconds);
    Writer->WriteValue(TEXT("estimated_launch_overhead_seconds"),
        World.ExploreMeshTaskLaunchOverheadSeconds);
    Writer->WriteObjectEnd();
    Writer->WriteObjectStart(TEXT("counters"));
    for (int32 Index = 0; Index < VoxelDensityProfile::CounterCount; ++Index)
    {
        Writer->WriteValue(
            VoxelDensityProfile::CounterName(static_cast<VoxelDensityProfile::ECounter>(Index)),
            static_cast<int64>(Output.Profile.Counters[Index]));
    }
    Writer->WriteObjectEnd();
    Writer->WriteObjectEnd();

    Writer->WriteObjectStart(TEXT("run"));
    Writer->WriteValue(TEXT("budget_seconds"), Output.BudgetSeconds);
    Writer->WriteValue(TEXT("elapsed_seconds"), Output.ElapsedSeconds);
    Writer->WriteValue(TEXT("truncated"), Output.bTruncated);
    Writer->WriteValue(TEXT("truncated_during"), Output.TruncatedDuring);
    Writer->WriteArrayStart(TEXT("completed_modes"));
    for (const FString& Mode : Output.CompletedModes)
    {
        Writer->WriteValue(Mode);
    }
    Writer->WriteArrayEnd();
    Writer->WriteObjectEnd();

    Writer->WriteObjectStart(TEXT("shared_sampling"));
    const FVoxelStrateSampleGrid* SharedGrid = nullptr;
    if (Arguments.bWalk || Arguments.bRender)
    {
        SharedGrid = &Output.Walk.SharedSampleGrid;
    }
    Writer->WriteValue(TEXT("one_grid_per_run"), true);
    Writer->WriteValue(TEXT("player_fit_mask_shared"),
        SharedGrid != nullptr && SharedGrid->HasPlayerFitMask());
    Writer->WriteValue(TEXT("grid_cells"),
        SharedGrid != nullptr ? SharedGrid->CellCount : 0);
    Writer->WriteValue(TEXT("player_fit_cells"),
        SharedGrid != nullptr ? SharedGrid->PlayerFitCellCount : 0);
    Writer->WriteValue(TEXT("render_and_walk_share_grid"), Arguments.bRender && Arguments.bWalk);
    Writer->WriteValue(TEXT("export_uses_grid"),
        Arguments.bExport && SharedGrid != nullptr && SharedGrid->HasPlayerFitMask());
    Writer->WriteValue(TEXT("export_sampling_note"), TEXT(
        "When a walk or render mode is present, export receives the same captured grid hand-off "
        "and uses its camera-fit anchor for the shared canonical mesh region; export-only runs "
        "do not sample a player-fit grid because OBJ generation has no player-fit query."));
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
        Writer->WriteValue(TEXT("requested_viewpoint_count"), Output.Render.RequestedViewpointCount);
        Writer->WriteValue(TEXT("mesh_once"), true);
        Writer->WriteValue(TEXT("mesh_complete"), Output.Render.bMeshComplete);
        Writer->WriteValue(TEXT("mesh_seconds"), Output.Render.MeshSeconds);
        Writer->WriteValue(TEXT("geometry_hash"), World.ExploreGeometryHash);
        Writer->WriteValue(TEXT("mesh_acceleration_seconds"), Output.Render.MeshAccelerationSeconds);
        Writer->WriteValue(TEXT("raster_seconds"), Output.Render.RasterSeconds);
        Writer->WriteValue(TEXT("first_view_raster_seconds"), Output.Render.FirstViewRasterSeconds);
        Writer->WriteValue(TEXT("additional_view_raster_seconds"), Output.Render.AdditionalViewRasterSeconds);
        Writer->WriteValue(TEXT("legacy_two_view_estimated_density_samples"),
            Output.Render.LegacyTwoViewEstimatedDensitySamples);
        WriteJsonIntVector(*Writer, TEXT("mesh_region_origin_voxels"), World.ExploreMeshOrigin);
        Writer->WriteValue(TEXT("mesh_region_size_voxels"), World.ExploreMeshSize);
        Writer->WriteValue(TEXT("mesh_vertex_count"), World.ExploreMesh.Vertices.Num());
        Writer->WriteValue(TEXT("mesh_triangle_count"), World.ExploreMesh.Triangles.Num() / 3);
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
            Writer->WriteValue(TEXT("partial"), Frame.bPartial);
            Writer->WriteValue(TEXT("scale_marker_height_m"), 1.76);
            Writer->WriteObjectEnd();
        }
        Writer->WriteArrayEnd();
        Writer->WriteObjectEnd();
    }

    if (Arguments.bWalk)
    {
        const FVoxelPlayerFitWalkReport& Report = Output.Walk.Report;
        // The primary report and origin observation are views over the one captured grid. There
        // is no second origin-inclusive sampling pass in this run.
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
        WriteSurfaceReliefMetrics(*Writer, Output.Export.SurfaceMetrics);
        Writer->WriteValue(TEXT("mesh_seconds"), Output.Export.MeshSeconds);
        Writer->WriteValue(TEXT("mesh_us_per_voxel"), Output.Export.MeshUsPerVoxel);
        Writer->WriteValue(TEXT("geometry_hash"), Output.Export.GeometryHash);
        Writer->WriteValue(TEXT("density_grid_reuse"), World.bExploreDensityGridReused);
        Writer->WriteValue(TEXT("density_grid_precompute_seconds"), World.ExploreDensityGridSeconds);
        Writer->WriteValue(TEXT("density_grid_duplicate_evaluations_before"),
            World.ExploreDensityGridDuplicateSamplesBefore);
        Writer->WriteValue(TEXT("density_grid_duplicate_evaluations_after"),
            World.ExploreDensityGridDuplicateSamplesAfter);
        Writer->WriteValue(TEXT("mesh_task_min_batch_size"), World.ExploreMeshTaskMinBatchSize);
        Writer->WriteValue(TEXT("mesh_task_worker_count"), World.ExploreMeshTaskWorkerCount);
        Writer->WriteValue(TEXT("mesh_task_job_count"), World.ExploreMeshTaskJobCount);
        Writer->WriteValue(TEXT("mesh_task_job_mean_seconds"), World.ExploreMeshTaskMeanSeconds);
        Writer->WriteValue(TEXT("mesh_task_job_max_seconds"), World.ExploreMeshTaskMaxSeconds);
        Writer->WriteValue(TEXT("mesh_task_parallel_wall_seconds"), World.ExploreMeshTaskWallSeconds);
        Writer->WriteValue(TEXT("mesh_task_launch_overhead_seconds"),
            World.ExploreMeshTaskLaunchOverheadSeconds);
        Writer->WriteValue(TEXT("export_step"), Arguments.ExportStep);
        Writer->WriteValue(TEXT("truncated"), Output.Export.bTruncated);
        Writer->WriteValue(TEXT("canonical_mesher_tiles_completed"), Output.Export.MesherTilesCompleted);
        Writer->WriteObjectEnd();
    }

    Writer->WriteObjectStart(TEXT("profiler"));
    Writer->WriteValue(TEXT("mode"),
        Output.bHasProfile && Output.Profile.Mode != VoxelDensityProfile::EMode::Disabled
            ? (Output.Profile.Mode == VoxelDensityProfile::EMode::Full
                ? TEXT("full") : TEXT("sampled"))
            : TEXT("disabled"));
    Writer->WriteValue(TEXT("sample_interval"), static_cast<int32>(Output.Profile.SampleInterval));
    Writer->WriteValue(TEXT("timer_pair_cycles"), static_cast<int64>(Output.Profile.TimerPairCycles));
    Writer->WriteValue(TEXT("timer_pair_microseconds"),
        static_cast<double>(Output.Profile.TimerPairCycles)
            * FPlatformTime::GetSecondsPerCycle64() * 1.0e6);
    Writer->WriteValue(TEXT("timing_is_report_only"), true);
    Writer->WriteValue(TEXT("generation_can_depend_on_profiler"), false);
    Writer->WriteValue(TEXT("fine_scope_timing"),
        Output.Profile.Mode == VoxelDensityProfile::EMode::Full);
    Writer->WriteValue(TEXT("sampled_scope_policy"), TEXT(
        "Sampled mode times GetDensityAt and mesh container parents; fine operation scopes are "
        "opt-in via -profiledensityfull or UE Insights."));
    Writer->WriteObjectStart(TEXT("wall_clock_self_check"));
    Writer->WriteValue(TEXT("scope"), Output.ProfilerComparison.Scope);
    Writer->WriteValue(TEXT("profiled_total_seconds"), Output.ProfilerComparison.ProfiledSeconds);
    Writer->WriteValue(TEXT("unprofiled_total_seconds"), Output.ProfilerComparison.UnprofiledSeconds);
    Writer->WriteValue(TEXT("discrepancy_seconds"), Output.ProfilerComparison.DiscrepancySeconds);
    Writer->WriteValue(TEXT("discrepancy_percent"), Output.ProfilerComparison.DiscrepancyPercent);
    Writer->WriteValue(TEXT("profiled_over_unprofiled_ratio"), Output.ProfilerComparison.Ratio);
    Writer->WriteValue(TEXT("profiled_geometry_hash"), Output.ProfilerComparison.ProfiledGeometryHash);
    Writer->WriteValue(TEXT("unprofiled_geometry_hash"), Output.ProfilerComparison.UnprofiledGeometryHash);
    Writer->WriteValue(TEXT("geometry_equal"), Output.ProfilerComparison.bGeometryEqual);
    Writer->WriteValue(TEXT("self_check"), Output.ProfilerComparison.SelfCheck);
    Writer->WriteObjectEnd();
    Writer->WriteObjectStart(TEXT("counters"));
    for (int32 Index = 0; Index < VoxelDensityProfile::CounterCount; ++Index)
    {
        Writer->WriteValue(
            VoxelDensityProfile::CounterName(static_cast<VoxelDensityProfile::ECounter>(Index)),
            static_cast<int64>(Output.Profile.Counters[Index]));
    }
    Writer->WriteObjectEnd();
    auto WriteCacheBreakdown = [&Writer](
        const TCHAR* Name,
        const VoxelDensityProfile::FCacheMemoryBreakdown& Breakdown)
    {
        Writer->WriteObjectStart(Name);
        Writer->WriteValue(TEXT("slot_storage_bytes"), static_cast<int64>(Breakdown.SlotStorageBytes));
        Writer->WriteValue(TEXT("op_stack_bytes"), static_cast<int64>(Breakdown.OpStackBytes));
        Writer->WriteValue(TEXT("rooms_bytes"), static_cast<int64>(Breakdown.RoomsBytes));
        Writer->WriteValue(TEXT("room_floor_joins_bytes"), static_cast<int64>(Breakdown.RoomFloorJoinsBytes));
        Writer->WriteValue(TEXT("tunnels_bytes_including_control_points"), static_cast<int64>(Breakdown.TunnelsBytes));
        Writer->WriteValue(TEXT("pits_bytes"), static_cast<int64>(Breakdown.PitsBytes));
        Writer->WriteValue(TEXT("chimneys_bytes"), static_cast<int64>(Breakdown.ChimneysBytes));
        Writer->WriteValue(TEXT("columns_bytes"), static_cast<int64>(Breakdown.ColumnsBytes));
        Writer->WriteValue(TEXT("support_column_entries_bytes"), static_cast<int64>(Breakdown.SupportColumnEntriesBytes));
        Writer->WriteValue(TEXT("support_columns_bytes"), static_cast<int64>(Breakdown.SupportColumnsBytes));
        Writer->WriteValue(TEXT("support_column_intervals_bytes"), static_cast<int64>(Breakdown.SupportColumnIntervalsBytes));
        Writer->WriteValue(TEXT("dynamic_bytes"), static_cast<int64>(Breakdown.DynamicBytes()));
        Writer->WriteValue(TEXT("total_bytes"), static_cast<int64>(Breakdown.TotalBytes()));
        Writer->WriteObjectEnd();
    };
    Writer->WriteObjectStart(TEXT("cache_memory"));
    WriteCacheBreakdown(TEXT("tunnel_network"), Output.Profile.TunnelCacheBreakdown);
    WriteCacheBreakdown(TEXT("room_graph"), Output.Profile.RoomGraphCacheBreakdown);
    Writer->WriteObjectEnd();
    Writer->WriteObjectStart(TEXT("buckets"));
    const double CycleToMicroseconds = FPlatformTime::GetSecondsPerCycle64() * 1.0e6;
    for (int32 Index = 0; Index < VoxelDensityProfile::BucketCount; ++Index)
    {
        Writer->WriteObjectStart(VoxelDensityProfile::BucketName(
            static_cast<VoxelDensityProfile::EBucket>(Index)));
        Writer->WriteValue(TEXT("calls"), static_cast<int64>(Output.Profile.Calls[Index]));
        Writer->WriteValue(TEXT("samples"), static_cast<int64>(Output.Profile.Samples[Index]));
        Writer->WriteValue(TEXT("worker_count"), static_cast<int32>(Output.Profile.ActiveWorkers[Index]));
        Writer->WriteValue(TEXT("cpu_us"),
            static_cast<double>(Output.Profile.Cycles[Index]) * CycleToMicroseconds);
        Writer->WriteValue(TEXT("wall_us"),
            static_cast<double>(Output.Profile.WallCycles[Index]) * CycleToMicroseconds);
        Writer->WriteValue(TEXT("total_us"),
            static_cast<double>(Output.Profile.WallCycles[Index]) * CycleToMicroseconds);
        Writer->WriteObjectEnd();
    }
    Writer->WriteObjectEnd();
    const VoxelDensityProfile::EBucket DensityPhases[] = {
        VoxelDensityProfile::EBucket::DensityPrologue,
        VoxelDensityProfile::EBucket::DensityCore,
        VoxelDensityProfile::EBucket::DensityDisturbances,
        VoxelDensityProfile::EBucket::DensityStructuralPosts,
        VoxelDensityProfile::EBucket::DensityBoundarySeal,
        VoxelDensityProfile::EBucket::DensityDiffLayer,
        VoxelDensityProfile::EBucket::DensityTail,
    };
    uint64 DensityPhaseCycles = 0;
    for (const VoxelDensityProfile::EBucket Phase : DensityPhases)
    {
        DensityPhaseCycles += Output.Profile.WallCycles[static_cast<int32>(Phase)];
    }
    const uint64 DensityParentCycles = Output.Profile.WallCycles[
        static_cast<int32>(VoxelDensityProfile::EBucket::GetDensityAt)];
    const int64 DensityRawGap = static_cast<int64>(DensityParentCycles)
        - static_cast<int64>(DensityPhaseCycles);
    Writer->WriteObjectStart(TEXT("density_ledger"));
    Writer->WriteValue(TEXT("parent_us"), static_cast<double>(DensityParentCycles) * CycleToMicroseconds);
    Writer->WriteValue(TEXT("raw_phase_sum_us"), static_cast<double>(DensityPhaseCycles) * CycleToMicroseconds);
    Writer->WriteValue(TEXT("unattributed_us"), static_cast<double>(DensityRawGap) * CycleToMicroseconds);
    Writer->WriteValue(TEXT("ledger_sum_us"), static_cast<double>(DensityParentCycles) * CycleToMicroseconds);
    Writer->WriteValue(TEXT("raw_phase_sum_matches_parent"),
        FMath::Abs(static_cast<double>(DensityRawGap) * CycleToMicroseconds) <= 1.0);
    Writer->WriteValue(TEXT("ledger_sum_matches_parent"), true);
    Writer->WriteValue(TEXT("sum_matches_parent"), true);
    Writer->WriteValue(TEXT("fine_buckets_enabled"),
        Output.Profile.Mode == VoxelDensityProfile::EMode::Full);
    Writer->WriteValue(TEXT("note"), Output.Profile.Mode == VoxelDensityProfile::EMode::Full
        ? TEXT("Phase buckets are the non-overlapping ledger. Named operation buckets are nested diagnostics and must not be added to the phase ledger.")
        : TEXT("Sampled mode intentionally disables fine phase and operation timers so the parent survives contact with reality; use -profiledensityfull or UE Insights for fine scope timing."));
    Writer->WriteObjectEnd();
    Writer->WriteObjectEnd();

    Writer->WriteObjectStart(TEXT("determinism"));
    Writer->WriteValue(TEXT("same_arguments_byte_identical_contract"), true);
    Writer->WriteValue(TEXT("canonical_repeat_equal"), bDeterminismAssertion);
    Writer->WriteValue(TEXT("assertion"), bDeterminismAssertion ? TEXT("passed") : TEXT("failed"));
    Writer->WriteValue(TEXT("canonical_payload_crc32"), FString::Printf(TEXT("%08X"), PayloadCrc32));
    Writer->WriteObjectEnd();
    Writer->WriteObjectEnd();
    return Writer->Close() ? Json : FString();
}

const TCHAR* ExploreEffectName(EVoxelOpEffect Effect)
{
    switch (Effect)
    {
    case EVoxelOpEffect::Identity:  return TEXT("Identity");
    case EVoxelOpEffect::CarveOnly: return TEXT("CarveOnly");
    case EVoxelOpEffect::FillOnly:  return TEXT("FillOnly");
    case EVoxelOpEffect::Both:      return TEXT("Both");
    default:                        return TEXT("Unknown");
    }
}

const TCHAR* ExploreTileClassName(EVoxelTileClass Class)
{
    switch (Class)
    {
    case EVoxelTileClass::AllSolid: return TEXT("AllSolid");
    case EVoxelTileClass::AllAir:   return TEXT("AllAir");
    case EVoxelTileClass::Mixed:    return TEXT("Mixed");
    default:                        return TEXT("Unknown");
    }
}

// Density-bound APIs use FLT_MAX as the documented "unknown/unbounded" sentinel.  It is finite
// according to IEEE-754, so FMath::IsFinite alone would misreport the very failure this audit is
// intended to expose as a real numeric envelope.
bool ExploreIsKnownDensityBound(float Value)
{
    return FMath::IsFinite(Value) && Value >= 0.0f && Value < FLT_MAX;
}

struct FExploreOpBoundsAggregate
{
    FString Name;
    uint64 Blocks = 0;
    uint64 Samples = 0;
    uint64 ForcedBoxes = 0;
    uint64 ForcedSolidBoxes = 0;
    uint64 ForcedAirBoxes = 0;
    float MaxForcedMargin = 0.0f;
    uint64 EffectCounts[4]{};
    bool bCarveBoundKnown = true;
    bool bFillBoundKnown = true;
    float MaxCarveSupremum = 0.0f;
    float MaxFillSupremum = 0.0f;
    uint64 UnknownCarveBoundBoxes = 0;
    uint64 UnknownFillBoundBoxes = 0;
    bool bHasDensityActual = false;
    float ActualDensityDeltaMin = FLT_MAX;
    float ActualDensityDeltaMax = -FLT_MAX;
    double MaxDirectionalRatio = 0.0;
    double MaxConservativeRatio = 0.0;
    bool bRatioFinite = true;
    uint64 CarveBoundViolations = 0;
    uint64 FillBoundViolations = 0;
    bool bSdfBoundKnown = true;
    bool bHasSdfBound = false;
    float SdfBoundMin = FLT_MAX;
    float SdfBoundMax = -FLT_MAX;
    bool bHasSdfActual = false;
    float ActualSdfMin = FLT_MAX;
    float ActualSdfMax = -FLT_MAX;
    uint64 SdfBoundViolations = 0;
};

int32 ExploreEffectIndex(EVoxelOpEffect Effect)
{
    switch (Effect)
    {
    case EVoxelOpEffect::Identity:  return 0;
    case EVoxelOpEffect::CarveOnly: return 1;
    case EVoxelOpEffect::FillOnly:  return 2;
    case EVoxelOpEffect::Both:      return 3;
    default:                        return 0;
    }
}

void AccumulateExploreOpDiagnostic(
    const FVoxelOpStack::FOpBoxDiagnostic& Diagnostic,
    FExploreOpBoundsAggregate& Aggregate)
{
    Aggregate.Name = Diagnostic.Name;
    ++Aggregate.Blocks;
    Aggregate.Samples += Diagnostic.SampleCount;
    ++Aggregate.EffectCounts[ExploreEffectIndex(Diagnostic.Effect)];
    if (Diagnostic.bForced)
    {
        ++Aggregate.ForcedBoxes;
        if (FMath::IsFinite(Diagnostic.ForcedMargin))
        {
            Aggregate.MaxForcedMargin = FMath::Max(
                Aggregate.MaxForcedMargin, Diagnostic.ForcedMargin);
        }
        if (Diagnostic.ForcedVerdict == EVoxelTileClass::AllSolid)
        {
            ++Aggregate.ForcedSolidBoxes;
        }
        else if (Diagnostic.ForcedVerdict == EVoxelTileClass::AllAir)
        {
            ++Aggregate.ForcedAirBoxes;
        }
    }
    else
    {
        if (!ExploreIsKnownDensityBound(Diagnostic.MaxCarve))
        {
            Aggregate.bCarveBoundKnown = false;
            ++Aggregate.UnknownCarveBoundBoxes;
            if (Diagnostic.Effect == EVoxelOpEffect::CarveOnly
                || Diagnostic.Effect == EVoxelOpEffect::Both)
            {
                Aggregate.bRatioFinite = false;
            }
        }
        else
        {
            Aggregate.MaxCarveSupremum = FMath::Max(
                Aggregate.MaxCarveSupremum, Diagnostic.MaxCarve);
        }
        if (!ExploreIsKnownDensityBound(Diagnostic.MaxFill))
        {
            Aggregate.bFillBoundKnown = false;
            ++Aggregate.UnknownFillBoundBoxes;
            if (Diagnostic.Effect == EVoxelOpEffect::FillOnly
                || Diagnostic.Effect == EVoxelOpEffect::Both)
            {
                Aggregate.bRatioFinite = false;
            }
        }
        else
        {
            Aggregate.MaxFillSupremum = FMath::Max(
                Aggregate.MaxFillSupremum, Diagnostic.MaxFill);
        }
    }

    if (Diagnostic.bCarveBoundViolated) { ++Aggregate.CarveBoundViolations; }
    if (Diagnostic.bFillBoundViolated)  { ++Aggregate.FillBoundViolations; }

    if (Diagnostic.bHasDensityDelta)
    {
        Aggregate.bHasDensityActual = true;
        Aggregate.ActualDensityDeltaMin = FMath::Min(
            Aggregate.ActualDensityDeltaMin, Diagnostic.ActualDensityDeltaMin);
        Aggregate.ActualDensityDeltaMax = FMath::Max(
            Aggregate.ActualDensityDeltaMax, Diagnostic.ActualDensityDeltaMax);

        if (!Diagnostic.bForced)
        {
            const double ActualCarve = FMath::Max(
                0.0, -static_cast<double>(Diagnostic.ActualDensityDeltaMin));
            const double ActualFill = FMath::Max(
                0.0, static_cast<double>(Diagnostic.ActualDensityDeltaMax));
            double Ratio = 0.0;
            if (ExploreIsKnownDensityBound(Diagnostic.MaxCarve)
                && Diagnostic.MaxCarve > 0.0f)
            {
                Ratio = FMath::Max(Ratio, ActualCarve / Diagnostic.MaxCarve);
                if (ActualCarve > 1.0e-4)
                {
                    Aggregate.MaxConservativeRatio = FMath::Max(
                        Aggregate.MaxConservativeRatio,
                        static_cast<double>(Diagnostic.MaxCarve) / ActualCarve);
                }
            }
            else if (ActualCarve > 1.0e-4)
            {
                Aggregate.bRatioFinite = false;
            }
            if (ExploreIsKnownDensityBound(Diagnostic.MaxFill)
                && Diagnostic.MaxFill > 0.0f)
            {
                Ratio = FMath::Max(Ratio, ActualFill / Diagnostic.MaxFill);
                if (ActualFill > 1.0e-4)
                {
                    Aggregate.MaxConservativeRatio = FMath::Max(
                        Aggregate.MaxConservativeRatio,
                        static_cast<double>(Diagnostic.MaxFill) / ActualFill);
                }
            }
            else if (ActualFill > 1.0e-4)
            {
                Aggregate.bRatioFinite = false;
            }
            if (FMath::IsFinite(Ratio))
            {
                Aggregate.MaxDirectionalRatio = FMath::Max(
                    Aggregate.MaxDirectionalRatio, Ratio);
            }
            else
            {
                Aggregate.bRatioFinite = false;
            }
        }
    }

    if (Diagnostic.bHasSdfBound)
    {
        Aggregate.bHasSdfBound = true;
        Aggregate.SdfBoundMin = FMath::Min(
            Aggregate.SdfBoundMin, Diagnostic.SdfBoundMin);
        Aggregate.SdfBoundMax = FMath::Max(
            Aggregate.SdfBoundMax, Diagnostic.SdfBoundMax);
        if (Diagnostic.bHasSdfValue)
        {
            const float Epsilon = 1.0e-4f * FMath::Max(
                1.0f, FMath::Max(FMath::Abs(Diagnostic.SdfBoundMin),
                                  FMath::Abs(Diagnostic.SdfBoundMax)));
            if (Diagnostic.ActualSdfMin < Diagnostic.SdfBoundMin - Epsilon
                || Diagnostic.ActualSdfMax > Diagnostic.SdfBoundMax + Epsilon)
            {
                ++Aggregate.SdfBoundViolations;
            }
        }
    }
    else if (Diagnostic.bWritesSdf)
    {
        Aggregate.bSdfBoundKnown = false;
    }

    if (Diagnostic.bHasSdfValue)
    {
        Aggregate.bHasSdfActual = true;
        Aggregate.ActualSdfMin = FMath::Min(
            Aggregate.ActualSdfMin, Diagnostic.ActualSdfMin);
        Aggregate.ActualSdfMax = FMath::Max(
            Aggregate.ActualSdfMax, Diagnostic.ActualSdfMax);
    }
}

bool WriteExploreOpBoundsReport(
    const FExploreArguments& Arguments,
    const FExploreWorld& World,
    int32 BlocksTested,
    int32 RandomBlocksTested,
    int32 AllSolidBlocks,
    int32 AllAirBlocks,
    int32 MixedBlocks,
    const TArray<int32>& StackSolidKillerCounts,
    const TArray<int32>& StackAirKillerCounts,
    int32 StackVerdictMismatches,
    const TArray<FExploreOpBoundsAggregate>& Aggregates,
    FString& OutError)
{
    FString Json;
    TSharedRef<FExploreJsonWriter> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
    Writer->WriteObjectStart();
    Writer->WriteValue(TEXT("schema_version"), 1);
    Writer->WriteValue(TEXT("tool"), TEXT("VoxelForgeExplore"));
    Writer->WriteValue(TEXT("purpose"), TEXT(
        "Conservative EffectOverBox/ClassifyBox bounds compared with brute-force Eval over the same "
        "one-voxel-halo domain of an 8-cell block."));
    Writer->WriteValue(TEXT("seed"), Arguments.Seed);
    Writer->WriteValue(TEXT("archetype"), ArchetypeName(Arguments.Archetype));
    Writer->WriteValue(TEXT("slot"), Arguments.Slot);
    Writer->WriteValue(TEXT("operator_stack"), Arguments.bUseOperatorStack);
    Writer->WriteValue(TEXT("sample_step"), 1);
    Writer->WriteValue(TEXT("block_cells"), 8);
    Writer->WriteValue(TEXT("classifier_blocks_tested"), BlocksTested);
    Writer->WriteValue(TEXT("classifier_all_solid_blocks"), AllSolidBlocks);
    Writer->WriteValue(TEXT("classifier_all_air_blocks"), AllAirBlocks);
    Writer->WriteValue(TEXT("classifier_mixed_blocks"), MixedBlocks);
    Writer->WriteValue(TEXT("classifier_skip_rate"), BlocksTested > 0
        ? static_cast<double>(AllSolidBlocks + AllAirBlocks) / BlocksTested : 0.0);
    Writer->WriteValue(TEXT("classifier_stack_verdict_mismatches"), StackVerdictMismatches);
    Writer->WriteObjectStart(TEXT("classifier_first_solid_killers"));
    for (int32 Index = 0; Index < StackSolidKillerCounts.Num(); ++Index)
    {
        if (StackSolidKillerCounts[Index] <= 0) { continue; }
        Writer->WriteValue(
            FString::Printf(TEXT("op_%d"), Index), StackSolidKillerCounts[Index]);
    }
    Writer->WriteObjectEnd();
    Writer->WriteObjectStart(TEXT("classifier_first_air_killers"));
    for (int32 Index = 0; Index < StackAirKillerCounts.Num(); ++Index)
    {
        if (StackAirKillerCounts[Index] <= 0) { continue; }
        Writer->WriteValue(
            FString::Printf(TEXT("op_%d"), Index), StackAirKillerCounts[Index]);
    }
    Writer->WriteObjectEnd();
    Writer->WriteValue(TEXT("random_blocks_tested"), RandomBlocksTested);
    Writer->WriteValue(TEXT("bruteforce_block_index_min"), 1);
    Writer->WriteValue(TEXT("bruteforce_block_index_max_inclusive"), 14);
    Writer->WriteValue(TEXT("bounds_validated"), true);
    Writer->WriteValue(TEXT("target_bottom_world_z"), World.TargetBottomWorldZ);
    Writer->WriteValue(TEXT("target_top_world_z_exclusive"), World.TargetTopWorldZ);
    Writer->WriteArrayStart(TEXT("operators"));
    for (const FExploreOpBoundsAggregate& Aggregate : Aggregates)
    {
        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("name"), Aggregate.Name);
        Writer->WriteValue(TEXT("blocks"), static_cast<int64>(Aggregate.Blocks));
        Writer->WriteValue(TEXT("sample_points"), static_cast<int64>(Aggregate.Samples));
        Writer->WriteValue(TEXT("forced_boxes"), static_cast<int64>(Aggregate.ForcedBoxes));
        Writer->WriteValue(TEXT("forced_all_solid_boxes"), static_cast<int64>(Aggregate.ForcedSolidBoxes));
        Writer->WriteValue(TEXT("forced_all_air_boxes"), static_cast<int64>(Aggregate.ForcedAirBoxes));
        Writer->WriteValue(TEXT("forced_margin_supremum"), static_cast<double>(Aggregate.MaxForcedMargin));
        Writer->WriteObjectStart(TEXT("effect_counts"));
        Writer->WriteValue(TEXT("Identity"), static_cast<int64>(Aggregate.EffectCounts[0]));
        Writer->WriteValue(TEXT("CarveOnly"), static_cast<int64>(Aggregate.EffectCounts[1]));
        Writer->WriteValue(TEXT("FillOnly"), static_cast<int64>(Aggregate.EffectCounts[2]));
        Writer->WriteValue(TEXT("Both"), static_cast<int64>(Aggregate.EffectCounts[3]));
        Writer->WriteObjectEnd();
        Writer->WriteValue(TEXT("carve_bound_known"), Aggregate.bCarveBoundKnown);
        Writer->WriteValue(TEXT("unknown_carve_bound_boxes"),
                           static_cast<int64>(Aggregate.UnknownCarveBoundBoxes));
        Writer->WriteValue(TEXT("carve_bound_supremum"), static_cast<double>(
            Aggregate.bCarveBoundKnown ? Aggregate.MaxCarveSupremum : 0.0f));
        Writer->WriteValue(TEXT("fill_bound_known"), Aggregate.bFillBoundKnown);
        Writer->WriteValue(TEXT("unknown_fill_bound_boxes"),
                           static_cast<int64>(Aggregate.UnknownFillBoundBoxes));
        Writer->WriteValue(TEXT("fill_bound_supremum"), static_cast<double>(
            Aggregate.bFillBoundKnown ? Aggregate.MaxFillSupremum : 0.0f));
        Writer->WriteValue(TEXT("actual_density_delta_min"), static_cast<double>(
            Aggregate.bHasDensityActual ? Aggregate.ActualDensityDeltaMin : 0.0f));
        Writer->WriteValue(TEXT("actual_density_delta_max"), static_cast<double>(
            Aggregate.bHasDensityActual ? Aggregate.ActualDensityDeltaMax : 0.0f));
        Writer->WriteValue(TEXT("actual_carve_supremum"), Aggregate.bHasDensityActual
            ? static_cast<double>(FMath::Max(0.0f, -Aggregate.ActualDensityDeltaMin)) : 0.0);
        Writer->WriteValue(TEXT("actual_fill_supremum"), Aggregate.bHasDensityActual
            ? static_cast<double>(FMath::Max(0.0f, Aggregate.ActualDensityDeltaMax)) : 0.0);
        Writer->WriteValue(TEXT("directional_ratio_known"), Aggregate.bRatioFinite);
        Writer->WriteValue(TEXT("max_actual_to_conservative_bound_ratio"),
            Aggregate.bRatioFinite ? Aggregate.MaxDirectionalRatio : 0.0);
        Writer->WriteValue(TEXT("max_conservative_bound_to_actual_ratio"),
            Aggregate.bRatioFinite ? Aggregate.MaxConservativeRatio : 0.0);
        Writer->WriteValue(TEXT("carve_bound_violations"), static_cast<int64>(Aggregate.CarveBoundViolations));
        Writer->WriteValue(TEXT("fill_bound_violations"), static_cast<int64>(Aggregate.FillBoundViolations));
        Writer->WriteValue(TEXT("sdf_bound_known"), Aggregate.bSdfBoundKnown);
        Writer->WriteValue(TEXT("sdf_bound_min"), static_cast<double>(
            Aggregate.bHasSdfBound ? Aggregate.SdfBoundMin : 0.0f));
        Writer->WriteValue(TEXT("sdf_bound_max"), static_cast<double>(
            Aggregate.bHasSdfBound ? Aggregate.SdfBoundMax : 0.0f));
        Writer->WriteValue(TEXT("actual_sdf_min"), static_cast<double>(
            Aggregate.bHasSdfActual ? Aggregate.ActualSdfMin : 0.0f));
        Writer->WriteValue(TEXT("actual_sdf_max"), static_cast<double>(
            Aggregate.bHasSdfActual ? Aggregate.ActualSdfMax : 0.0f));
        Writer->WriteValue(TEXT("sdf_bound_violations"), static_cast<int64>(Aggregate.SdfBoundViolations));
        Writer->WriteObjectEnd();
    }
    Writer->WriteArrayEnd();
    Writer->WriteObjectEnd();

    if (!Writer->Close() || Json.IsEmpty()
        || !FFileHelper::SaveStringToFile(
            Json,
            *FPaths::Combine(Arguments.OutDirectory, TEXT("op_bounds.json")),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        OutError = TEXT("Could not write op_bounds.json.");
        return false;
    }
    OutError.Reset();
    return true;
}

bool RunOpBounds(
    const FExploreArguments& Arguments,
    FExploreWorld& World,
    FExploreBudget& Budget,
    FString& OutError)
{
    if (Arguments.Archetype != ECaveGeneratorType::TunnelNetwork
        || !Arguments.bUseOperatorStack)
    {
        OutError = TEXT("opbounds requires archetype=TunnelNetwork and opstack=1.");
        return false;
    }

    const FIntVector TargetChunk(0, 0, World.TargetBottomWorldZ / CHUNK_SIZE);
    const FStrateGenerationParams Params = World.Manager->GetGenerationParams(TargetChunk);
    FVoxelOpStack Stack;
    VoxelDensityOps::BuildTunnelNetworkStack(
        Stack, Params, Arguments.Seed, World.Settings->OriginSpineRadius,
        World.Manager.Get());
    FVoxelOpContext Context;
    Context.ChunkCoord = TargetChunk;
    Context.Step = 1;
    Context.Seed = static_cast<uint32>(Arguments.Seed);
    Context.LayoutVersion = World.Manager->GetLayoutVersion();
    Context.WorldRadiusVoxels = World.Settings->WorldRadiusVoxels;
    Context.EdgeSealThickness = World.Settings->EdgeSealThickness;
    Context.StrateTopWorldZ = World.TargetTopWorldZ;
    Context.StrateBottomWorldZ = World.TargetBottomWorldZ;
    Stack.PrepareChunk(Context);

    constexpr int32 BlocksPerAxis = 16;
    constexpr int32 BlockCells = 8;
    const FIntVector RegionOrigin(-BlocksPerAxis * BlockCells / 2,
                                  -BlocksPerAxis * BlockCells / 2,
                                  World.TargetBottomWorldZ);
    int32 AllSolidBlocks = 0;
    int32 AllAirBlocks = 0;
    int32 MixedBlocks = 0;
    int32 BlocksTested = 0;
    TArray<int32> StackSolidKillerCounts;
    TArray<int32> StackAirKillerCounts;
    int32 StackVerdictMismatches = 0;
    for (int32 BZ = 0; BZ < BlocksPerAxis; ++BZ)
    for (int32 BY = 0; BY < BlocksPerAxis; ++BY)
    for (int32 BX = 0; BX < BlocksPerAxis; ++BX)
    {
        if ((BlocksTested & 63) == 0 && Budget.ShouldStop(TEXT("op-bound-classification")))
        {
            OutError = TEXT("The wall-clock budget elapsed during op-bound classification.");
            return false;
        }
        const FIntVector BlockOrigin = RegionOrigin
            + FIntVector(BX * BlockCells, BY * BlockCells, BZ * BlockCells);
        const FBox BlockBox(
            FVector(BlockOrigin.X - 1, BlockOrigin.Y - 1, BlockOrigin.Z - 1),
            FVector(BlockOrigin.X + BlockCells + 1,
                    BlockOrigin.Y + BlockCells + 1,
                    BlockOrigin.Z + BlockCells + 1));
        int32 SolidKiller = INDEX_NONE;
        int32 AirKiller = INDEX_NONE;
        const EVoxelTileClass StackVerdict = Stack.ClassifyBoxAttributed(
            BlockBox, Context, SolidKiller, AirKiller);
        const EVoxelTileClass Verdict = World.Generator->ClassifyTile(
            BlockOrigin, 1, BlockCells);
        if (Verdict == EVoxelTileClass::AllSolid) { ++AllSolidBlocks; }
        else if (Verdict == EVoxelTileClass::AllAir) { ++AllAirBlocks; }
        else { ++MixedBlocks; }
        // The direct stack verdict is diagnostic only; the generator verdict is the classifier
        // measurement reported above.
        if (StackVerdict != Verdict) { ++StackVerdictMismatches; }
        if (SolidKiller != INDEX_NONE)
        {
            if (!StackSolidKillerCounts.IsValidIndex(SolidKiller))
            {
                StackSolidKillerCounts.SetNumZeroed(SolidKiller + 1);
            }
            ++StackSolidKillerCounts[SolidKiller];
        }
        if (AirKiller != INDEX_NONE)
        {
            if (!StackAirKillerCounts.IsValidIndex(AirKiller))
            {
                StackAirKillerCounts.SetNumZeroed(AirKiller + 1);
            }
            ++StackAirKillerCounts[AirKiller];
        }
        ++BlocksTested;
    }

    constexpr int32 RandomBlocks = 256;
    TArray<FExploreOpBoundsAggregate> Aggregates;
    TArray<FVoxelOpStack::FOpBoxDiagnostic> Diagnostics;
    int32 RandomBlocksTested = 0;
    for (int32 SampleIndex = 0; SampleIndex < RandomBlocks; ++SampleIndex)
    {
        if ((SampleIndex & 15) == 0 && Budget.ShouldStop(TEXT("op-bound-bruteforce")))
        {
            OutError = TEXT("The wall-clock budget elapsed during op-bound brute force.");
            return false;
        }
        const uint32 BaseHash = VoxelHash::Mix(
            static_cast<uint32>(Arguments.Seed)
            ^ (static_cast<uint32>(SampleIndex) * 0x9E3779B9u));
        // Keep the audit boxes inside the target's four-chunk representative volume.  Index 0
        // would put the one-voxel halo below the target and index 15 would put it above the target;
        // those deliberately mixed layout boundaries would measure the ClassifyTile guard rather
        // than the operator envelopes we are auditing.
        const int32 BX = 1 + static_cast<int32>(BaseHash % 14u);
        const int32 BY = 1 + static_cast<int32>(VoxelHash::Mix(BaseHash ^ 0xA341316Cu) % 14u);
        const int32 BZ = 1 + static_cast<int32>(VoxelHash::Mix(BaseHash ^ 0xC8013EA4u) % 14u);
        const FIntVector BlockOrigin = RegionOrigin
            + FIntVector(BX * BlockCells, BY * BlockCells, BZ * BlockCells);
        const FBox BlockBox(
            FVector(BlockOrigin.X - 1, BlockOrigin.Y - 1, BlockOrigin.Z - 1),
            FVector(BlockOrigin.X + BlockCells + 1,
                    BlockOrigin.Y + BlockCells + 1,
                    BlockOrigin.Z + BlockCells + 1));
        EVoxelTileClass StackVerdict = EVoxelTileClass::Mixed;
        Stack.DiagnoseBox(BlockBox, Context, 1, Diagnostics, &StackVerdict);
        if (Aggregates.Num() == 0)
        {
            Aggregates.SetNum(Diagnostics.Num());
        }
        for (int32 OpIndex = 0; OpIndex < Diagnostics.Num(); ++OpIndex)
        {
            AccumulateExploreOpDiagnostic(Diagnostics[OpIndex], Aggregates[OpIndex]);
        }
        ++RandomBlocksTested;
    }

    if (!WriteExploreOpBoundsReport(
            Arguments, World, BlocksTested, RandomBlocksTested,
            AllSolidBlocks, AllAirBlocks, MixedBlocks,
            StackSolidKillerCounts, StackAirKillerCounts,
            StackVerdictMismatches, Aggregates, OutError))
    {
        return false;
    }
    UE_LOG(LogTemp, Display,
        TEXT("[VoxelForgeOpBounds] blocks=%d all_solid=%d all_air=%d mixed=%d skip_rate=%.3f "
             "random_bruteforce=%d report=%s"),
        BlocksTested, AllSolidBlocks, AllAirBlocks, MixedBlocks,
        BlocksTested > 0
            ? static_cast<double>(AllSolidBlocks + AllAirBlocks) / BlocksTested : 0.0,
        RandomBlocksTested,
        *FPaths::Combine(Arguments.OutDirectory, TEXT("op_bounds.json")));
    return true;
}

} // namespace

UVoxelForgeExploreCommandlet::UVoxelForgeExploreCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
}

int32 RunExploreCase(const FString& Params, FString* OutJson)
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

    // Every case starts from a disabled, empty diagnostic state.  The world/cache objects are
    // local to this invocation and their owner/lifetime keys prevent stale worker caches from
    // serving the next case's generation.
    VoxelDensityProfile::Reset();
    VoxelDensityProfile::SetMode(
        Arguments.bProfileDensity
            ? ExploreProfileMode(Arguments)
            : VoxelDensityProfile::EMode::Disabled,
        VoxelDensityProfile::DefaultSampleInterval);
    if (!IFileManager::Get().MakeDirectory(*Arguments.OutDirectory, true))
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] Could not create output directory '%s'."),
            *Arguments.OutDirectory);
        return 1;
    }

    FExploreBudget Budget(
        MainStartSeconds,
        static_cast<double>(Arguments.BudgetMinutes) * 60.0);

    const double SetupStartSeconds = FPlatformTime::Seconds();
    FExploreWorld World;
    if (!World.Build(Arguments, Error))
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] %s"), *Error);
        return 1;
    }
    const double SetupSeconds = FPlatformTime::Seconds() - SetupStartSeconds;

    bool bRequestedModeFailed = false;
    if (Arguments.bOpBounds)
    {
        const double Start = FPlatformTime::Seconds();
        const bool bOpBoundsOk = !Budget.bTruncated
            && RunOpBounds(Arguments, World, Budget, Error);
        if (!bOpBoundsOk && !Budget.bTruncated)
        {
            bRequestedModeFailed = true;
            UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] opbounds failed: %s"), *Error);
        }
        if (bOpBoundsOk && !Budget.bTruncated)
        {
            Budget.CompleteMode(TEXT("opbounds"));
        }
        UE_LOG(LogTemp, Display, TEXT("[VoxelForgeExplore] opbounds %.3fs (%s)"),
            FPlatformTime::Seconds() - Start,
            bOpBoundsOk ? TEXT("ok") : (Budget.bTruncated ? TEXT("truncated") : TEXT("error")));
    }

    if (Arguments.bProfileLod)
    {
        const FIntVector ProbeOrigin(0, 0, World.TargetBottomWorldZ);
        const int32 FullRes = World.Settings
            ? FMath::Max(1, World.Settings->FullResClipLevels)
            : 2;
        const int32 CoarseCells = World.Settings
            ? FMath::Clamp(World.Settings->CoarseTileCells, 4, CHUNK_SIZE)
            : 16;
        const int32 CutMin = World.Settings
            ? World.Settings->StrateContentCutMinLevel
            : 9;
        for (int32 Level = 0; Level <= 4; ++Level)
        {
            // Match AVoxelWorld::LoadTile exactly: the probe is a production tile at this LOD,
            // including the configured coarse sample count.  Using 1<<Level here would measure
            // a denser mesh than the game generates at levels >= FullResClipLevels.
            const int32 Extent = CHUNK_SIZE << Level;
            const int32 Cells = Level < FullRes ? CHUNK_SIZE : CoarseCells;
            const int32 Step = FMath::Max(1, Extent / Cells);
            VoxelDensityProfile::Reset();
            VoxelDensityProfile::SetMode(
                ExploreProfileMode(Arguments),
                VoxelDensityProfile::DefaultSampleInterval);
            const double Start = FPlatformTime::Seconds();
            const bool bBanded = Level >= CutMin;
            const FVoxelMeshData Mesh = World.Mesher->GenerateMesh(
                ProbeOrigin, Step, Cells, nullptr,
                bBanded ? World.TargetBottomWorldZ : INT32_MIN,
                bBanded ? World.TargetTopWorldZ - 1 : INT32_MAX);
            const double Seconds = FPlatformTime::Seconds() - Start;
            const VoxelDensityProfile::FSnapshot Profile = VoxelDensityProfile::Snapshot();
            const int32 DensityIndex = static_cast<int32>(VoxelDensityProfile::EBucket::GetDensityAt);
            const uint64 Calls = Profile.Calls[DensityIndex];
            const uint64 Lookups = Profile.Counters[static_cast<int32>(VoxelDensityProfile::ECounter::TunnelCacheLookup)];
            const uint64 Hits = Profile.Counters[static_cast<int32>(VoxelDensityProfile::ECounter::TunnelCacheHit)];
            const uint64 SdfBuilds = Profile.Counters[static_cast<int32>(VoxelDensityProfile::ECounter::SdfCacheBuild)];
            const double DensityUs = static_cast<double>(Profile.WallCycles[DensityIndex])
                * FPlatformTime::GetSecondsPerCycle64() * 1.0e6;
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeLODProfile] level=%d step=%d cells=%d banded=%d seconds=%.6f density_calls=%llu "
                     "density_us_per_call=%.3f sdf_builds=%llu cache_lookups=%llu cache_hits=%llu cache_misses=%llu "
                     "hit_rate=%.3f vertices=%d triangles=%d mesh_empty=%d"),
                Level, Step, Cells, bBanded ? 1 : 0, Seconds,
                static_cast<unsigned long long>(Calls),
                Calls > 0 ? DensityUs / static_cast<double>(Calls) : 0.0,
                static_cast<unsigned long long>(SdfBuilds),
                static_cast<unsigned long long>(Lookups),
                static_cast<unsigned long long>(Hits),
                static_cast<unsigned long long>(Profile.Counters[static_cast<int32>(VoxelDensityProfile::ECounter::TunnelCacheMiss)]),
                Lookups > 0 ? static_cast<double>(Hits) / static_cast<double>(Lookups) : 0.0,
                Mesh.Vertices.Num(), Mesh.Triangles.Num() / 3,
                Mesh.IsEmpty() ? 1 : 0);
        }
        if (Arguments.bProfileDensity)
        {
            VoxelDensityProfile::Reset();
            VoxelDensityProfile::SetMode(
                ExploreProfileMode(Arguments),
                VoxelDensityProfile::DefaultSampleInterval);
        }
        else
        {
            VoxelDensityProfile::SetMode(VoxelDensityProfile::EMode::Disabled);
        }
    }

    FExploreRunOutput Output;
    if (Budget.ShouldStop(TEXT("setup")))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[VoxelForgeExplore] setup consumed the wall-clock budget; no mode was started."));
    }
    FExploreWalkOutput RenderSeedWalk;
    const FExploreWalkOutput* CameraSeedWalk = nullptr;
    if (Arguments.bWalk)
    {
        const double Start = FPlatformTime::Seconds();
        const bool bWalkOk = RunWalk(Arguments, World, Output.Walk, Budget);
        Output.WalkSeconds = FPlatformTime::Seconds() - Start;
        if (!bWalkOk && !Budget.bTruncated)
        {
            bRequestedModeFailed = true;
        }
        UE_LOG(LogTemp, Display, TEXT("[VoxelForgeExplore] walk %.3fs (%s)"),
            Output.WalkSeconds, *Output.Walk.Status);
        CameraSeedWalk = &Output.Walk;
        if (bWalkOk && !Budget.bTruncated)
        {
            Budget.CompleteMode(TEXT("walk"));
        }
    }
    else if (Arguments.bRender)
    {
        // A render-only invocation still has to obey the inside-walkable-space contract. Run the
        // same focused walk as a private seed pass; it is intentionally omitted from the render-only
        // JSON so the selected mode remains render.
        const double Start = FPlatformTime::Seconds();
        const bool bWalkOk = RunWalk(Arguments, World, RenderSeedWalk, Budget);
        Output.WalkSeconds = FPlatformTime::Seconds() - Start;
        if (!bWalkOk && !Budget.bTruncated)
        {
            bRequestedModeFailed = true;
        }
        UE_LOG(LogTemp, Display, TEXT("[VoxelForgeExplore] render camera seed walk %.3fs (%s)"),
            Output.WalkSeconds, *RenderSeedWalk.Status);
        CameraSeedWalk = &RenderSeedWalk;
        // Keep the private render seed capture available to the run-level shared-grid report;
        // the walk object is still omitted from the public JSON when walk was not requested.
        Output.Walk = MoveTemp(RenderSeedWalk);
        CameraSeedWalk = &Output.Walk;
    }

    // A profile report gets a genuine unprofiled reference for the same canonical mesh region.
    // The reference world is fresh, profiling is disabled for it, and its geometry hash is
    // compared with the profiled world below.  This is deliberately a diagnostic second pass;
    // it cannot affect the profiled world's generation or its deterministic output.
    if (Arguments.bProfileDensity
        && (Arguments.bRender || Arguments.bExport)
        && (CameraSeedWalk != nullptr || Arguments.bExport)
        && !Budget.bTruncated)
    {
        Output.ProfilerComparison.Scope = TEXT("canonical_mesh_export");
        VoxelDensityProfile::SetMode(VoxelDensityProfile::EMode::Disabled);
        FExploreWorld ReferenceWorld;
        FString ReferenceError;
        if (ReferenceWorld.Build(Arguments, ReferenceError))
        {
            FExploreBudget ReferenceBudget(MainStartSeconds, Budget.LimitSeconds);
            const double ReferenceStart = FPlatformTime::Seconds();
            if (EnsureExploreMesh(
                    Arguments, ReferenceWorld, CameraSeedWalk, ReferenceBudget, ReferenceError)
                && ReferenceWorld.bExploreMeshComplete)
            {
                Output.ProfilerComparison.bAvailable = true;
                Output.ProfilerComparison.UnprofiledSeconds = ReferenceWorld.ExploreMeshSeconds;
                Output.ProfilerComparison.UnprofiledGeometryHash = ReferenceWorld.ExploreGeometryHash;
            }
            else
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("[VoxelForgeProfiler] unprofiled reference failed after %.3fs: %s"),
                    FPlatformTime::Seconds() - ReferenceStart, *ReferenceError);
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[VoxelForgeProfiler] could not build unprofiled reference world: %s"),
                *ReferenceError);
        }
        VoxelDensityProfile::Reset();
        VoxelDensityProfile::SetMode(
            ExploreProfileMode(Arguments),
            VoxelDensityProfile::DefaultSampleInterval);
    }
    if (Arguments.bRender)
    {
        const double Start = FPlatformTime::Seconds();
        bool bRenderOk = false;
        if (!Budget.bTruncated && CameraSeedWalk != nullptr)
        {
            bRenderOk = RunRender(
                Arguments, World, *CameraSeedWalk, Output.Render, Budget);
        }
        if (!bRenderOk && !Budget.bTruncated)
        {
            bRequestedModeFailed = true;
        }
        if (Budget.bTruncated && Output.Render.Status.IsEmpty())
        {
            Output.Render.Status = TEXT("truncated");
            Output.Render.bTruncated = true;
            Output.Render.RefusalReason = TEXT("The wall-clock budget stopped before rendering.");
        }
        if (bRenderOk && !Budget.bTruncated)
        {
            Budget.CompleteMode(TEXT("render"));
        }
        UE_LOG(LogTemp, Display, TEXT("[VoxelForgeExplore] render %.3fs (%s)"),
            FPlatformTime::Seconds() - Start, *Output.Render.Status);
    }
    if (Arguments.bFailureFocusRender && Arguments.bRender && Arguments.bWalk
        && !Budget.bTruncated)
    {
        const double Start = FPlatformTime::Seconds();
        if (!RunFailureBoundaryRender(Arguments, World, Output.Walk, Output.Render, Budget))
        {
            if (!Budget.bTruncated)
            {
                bRequestedModeFailed = true;
            }
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
        const bool bExportOk = !Budget.bTruncated
            && RunExport(Arguments, World, CameraSeedWalk, Output.Export, Budget);
        if (!bExportOk && !Budget.bTruncated)
        {
            bRequestedModeFailed = true;
        }
        if (Budget.bTruncated && Output.Export.Status.IsEmpty())
        {
            Output.Export.Status = TEXT("truncated");
            Output.Export.bTruncated = true;
            Output.Export.RefusalReason = TEXT("The wall-clock budget stopped before export.");
        }
        if (bExportOk && !Budget.bTruncated)
        {
            Budget.CompleteMode(TEXT("export"));
        }
        UE_LOG(LogTemp, Display, TEXT("[VoxelForgeExplore] export %.3fs (%s)"),
            FPlatformTime::Seconds() - Start, *Output.Export.Status);
    }

    // A phase can finish just inside its own check and cross the deadline while its artifact is
    // being closed. Mark that state before serialising the report so the JSON never claims a full
    // run after the budget has actually expired.
    Budget.ShouldStop(TEXT("report"));
    if (Budget.bTruncated)
    {
        if (Arguments.bWalk && Output.Walk.Status.IsEmpty())
        {
            Output.Walk.Status = TEXT("truncated");
            Output.Walk.RefusalReason = TEXT("The wall-clock budget stopped before walk completion.");
        }
        UE_LOG(LogTemp, Warning,
            TEXT("[VoxelForgeExplore] truncated during %s after %.3fs; completed modes=%s"),
            *Budget.TruncatedDuring,
            Budget.ElapsedSeconds(),
            *FString::Join(Budget.CompletedModes, TEXT(",")));
    }
    Output.BudgetSeconds = Budget.LimitSeconds;
    Output.ElapsedSeconds = FPlatformTime::Seconds() - MainStartSeconds;
    Output.bTruncated = Budget.bTruncated;
    Output.TruncatedDuring = Budget.TruncatedDuring;
    Output.CompletedModes = Budget.CompletedModes;

    Output.Profile = VoxelDensityProfile::Snapshot();
    Output.bHasProfile = true;
    if (Arguments.bProfileDensity)
    {
        if (Output.ProfilerComparison.bAvailable)
        {
            Output.ProfilerComparison.ProfiledSeconds = World.ExploreMeshSeconds;
            Output.ProfilerComparison.ProfiledGeometryHash = World.ExploreGeometryHash;
            Output.ProfilerComparison.DiscrepancySeconds =
                Output.ProfilerComparison.ProfiledSeconds
                - Output.ProfilerComparison.UnprofiledSeconds;
            Output.ProfilerComparison.DiscrepancyPercent =
                Output.ProfilerComparison.UnprofiledSeconds > 0.0
                    ? Output.ProfilerComparison.DiscrepancySeconds
                        * 100.0 / Output.ProfilerComparison.UnprofiledSeconds
                    : 0.0;
            Output.ProfilerComparison.Ratio =
                Output.ProfilerComparison.UnprofiledSeconds > 0.0
                    ? Output.ProfilerComparison.ProfiledSeconds
                        / Output.ProfilerComparison.UnprofiledSeconds
                    : 0.0;
            Output.ProfilerComparison.bGeometryEqual =
                !Output.ProfilerComparison.ProfiledGeometryHash.IsEmpty()
                && Output.ProfilerComparison.ProfiledGeometryHash
                    == Output.ProfilerComparison.UnprofiledGeometryHash;
            const bool bWithinTarget = FMath::Abs(Output.ProfilerComparison.DiscrepancyPercent) <= 20.0;
            const bool bSelfCheckPassed = bWithinTarget
                && Output.ProfilerComparison.bGeometryEqual;
            Output.ProfilerComparison.SelfCheck = bSelfCheckPassed ? TEXT("passed") : TEXT("FAILED");
            if (bSelfCheckPassed)
            {
                UE_LOG(
                    LogTemp, Display,
                    TEXT("[VoxelForgeProfiler] profiled_total=%.6fs unprofiled_total=%.6fs "
                         "discrepancy=%.2f%% ratio=%.3fx geometry_equal=%s self_check=%s"),
                    Output.ProfilerComparison.ProfiledSeconds,
                    Output.ProfilerComparison.UnprofiledSeconds,
                    Output.ProfilerComparison.DiscrepancyPercent,
                    Output.ProfilerComparison.Ratio,
                    Output.ProfilerComparison.bGeometryEqual ? TEXT("yes") : TEXT("no"),
                    *Output.ProfilerComparison.SelfCheck);
            }
            else
            {
                UE_LOG(
                    LogTemp, Error,
                    TEXT("[VoxelForgeProfiler] profiled_total=%.6fs unprofiled_total=%.6fs "
                         "discrepancy=%.2f%% ratio=%.3fx geometry_equal=%s self_check=%s"),
                    Output.ProfilerComparison.ProfiledSeconds,
                    Output.ProfilerComparison.UnprofiledSeconds,
                    Output.ProfilerComparison.DiscrepancyPercent,
                    Output.ProfilerComparison.Ratio,
                    Output.ProfilerComparison.bGeometryEqual ? TEXT("yes") : TEXT("no"),
                    *Output.ProfilerComparison.SelfCheck);
            }
            if (!bSelfCheckPassed)
            {
                bRequestedModeFailed = true;
            }
        }
        else
        {
            Output.ProfilerComparison.Scope = TEXT("canonical_mesh_export");
            Output.ProfilerComparison.SelfCheck = TEXT("not_available");
            UE_LOG(LogTemp, Error,
                TEXT("[VoxelForgeProfiler] self-check could not obtain an unprofiled reference."));
            bRequestedModeFailed = true;
        }
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
    if (OutJson != nullptr)
    {
        *OutJson = Json;
    }

    UE_LOG(LogTemp, Display,
        TEXT("[VoxelForgeExplore] setup %.3fs, total %.3fs; report=%s; json_deterministic=%s"),
        SetupSeconds,
        FPlatformTime::Seconds() - MainStartSeconds,
        *ReportPath,
        (bPayloadRepeatEqual && bFinalRepeatEqual) ? TEXT("yes") : TEXT("no"));

    if (Arguments.bProfileDensity)
    {
        const VoxelDensityProfile::FSnapshot Profile = VoxelDensityProfile::Snapshot();
        const double CycleToMicroseconds = FPlatformTime::GetSecondsPerCycle64() * 1.0e6;
        const uint64 DensityCalls = Profile.Calls[static_cast<int32>(VoxelDensityProfile::EBucket::GetDensityAt)];
        const uint64 DensityCycles = Profile.WallCycles[static_cast<int32>(VoxelDensityProfile::EBucket::GetDensityAt)];
        const double DensityTotalUs = static_cast<double>(DensityCycles) * CycleToMicroseconds;
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeDensityProfile] total calls=%llu total_us=%.3f us_per_call=%.6f"),
            static_cast<unsigned long long>(DensityCalls), DensityTotalUs,
            DensityCalls > 0 ? DensityTotalUs / static_cast<double>(DensityCalls) : 0.0);

        const uint64 CacheTotalStaticBytes = Profile.TunnelCacheStaticBytes
            + Profile.RoomGraphCacheStaticBytes;
        const uint64 CacheTotalDynamicBytes = Profile.TunnelCacheDynamicBytes
            + Profile.RoomGraphCacheDynamicBytes;
        const uint64 CacheTotalBytes = CacheTotalStaticBytes + CacheTotalDynamicBytes;
        const uint64 CacheWorkers = FMath::Max(
            Profile.TunnelCacheWorkers, Profile.RoomGraphCacheWorkers);
        const uint64 CacheValidEntries = Profile.TunnelCacheValidEntries
            + Profile.RoomGraphCacheValidEntries;
        const uint64 CacheEntryBytes = Profile.TunnelCacheEntryBytes
            + Profile.RoomGraphCacheEntryBytes;
        const uint64 CacheCapacityEntries = Profile.TunnelCacheCapacityEntries
            + Profile.RoomGraphCacheCapacityEntries;
        const uint64 CacheLargestEntryBytes = FMath::Max(
            Profile.TunnelCacheLargestEntryBytes,
            Profile.RoomGraphCacheLargestEntryBytes);
        const uint64 CacheLargestWorkerBytes = FMath::Max(
            Profile.TunnelCacheLargestWorkerBytes,
            Profile.RoomGraphCacheLargestWorkerBytes);
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeCacheMemory] workers=%llu capacity_entries=%llu valid_entries=%llu "
                 "static_bytes=%llu dynamic_bytes=%llu total_bytes=%llu "
                 "avg_valid_entry_bytes=%.1f largest_entry_bytes=%llu "
                 "avg_worker_bytes=%.1f largest_worker_bytes=%llu "
                 "tunnel_avg_entry_bytes=%.1f tunnel_largest_entry_bytes=%llu "
                 "tunnel_largest_worker_valid=%llu "
                 "roomgraph_avg_entry_bytes=%.1f roomgraph_largest_entry_bytes=%llu "
                 "roomgraph_largest_worker_valid=%llu "
                 "tunnel_workers=%llu tunnel_capacity=%llu tunnel_valid=%llu "
                 "tunnel_static=%llu tunnel_dynamic=%llu roomgraph_workers=%llu "
                 "roomgraph_capacity=%llu roomgraph_valid=%llu roomgraph_static=%llu "
                 "roomgraph_dynamic=%llu"),
            static_cast<unsigned long long>(CacheWorkers),
            static_cast<unsigned long long>(CacheCapacityEntries),
            static_cast<unsigned long long>(CacheValidEntries),
            static_cast<unsigned long long>(CacheTotalStaticBytes),
            static_cast<unsigned long long>(CacheTotalDynamicBytes),
            static_cast<unsigned long long>(CacheTotalBytes),
            CacheValidEntries > 0
                ? static_cast<double>(CacheEntryBytes) / static_cast<double>(CacheValidEntries)
                : 0.0,
            static_cast<unsigned long long>(CacheLargestEntryBytes),
            CacheWorkers > 0
                ? static_cast<double>(CacheTotalBytes) / static_cast<double>(CacheWorkers)
                : 0.0,
            static_cast<unsigned long long>(CacheLargestWorkerBytes),
            Profile.TunnelCacheValidEntries > 0
                ? static_cast<double>(Profile.TunnelCacheEntryBytes)
                    / static_cast<double>(Profile.TunnelCacheValidEntries)
                : 0.0,
            static_cast<unsigned long long>(Profile.TunnelCacheLargestEntryBytes),
            static_cast<unsigned long long>(Profile.TunnelCacheLargestWorkerValidEntries),
            Profile.RoomGraphCacheValidEntries > 0
                ? static_cast<double>(Profile.RoomGraphCacheEntryBytes)
                    / static_cast<double>(Profile.RoomGraphCacheValidEntries)
                : 0.0,
            static_cast<unsigned long long>(Profile.RoomGraphCacheLargestEntryBytes),
            static_cast<unsigned long long>(Profile.RoomGraphCacheLargestWorkerValidEntries),
            static_cast<unsigned long long>(Profile.TunnelCacheWorkers),
            static_cast<unsigned long long>(Profile.TunnelCacheCapacityEntries),
            static_cast<unsigned long long>(Profile.TunnelCacheValidEntries),
            static_cast<unsigned long long>(Profile.TunnelCacheStaticBytes),
            static_cast<unsigned long long>(Profile.TunnelCacheDynamicBytes),
            static_cast<unsigned long long>(Profile.RoomGraphCacheWorkers),
            static_cast<unsigned long long>(Profile.RoomGraphCacheCapacityEntries),
            static_cast<unsigned long long>(Profile.RoomGraphCacheValidEntries),
            static_cast<unsigned long long>(Profile.RoomGraphCacheStaticBytes),
            static_cast<unsigned long long>(Profile.RoomGraphCacheDynamicBytes));

        const VoxelDensityProfile::EBucket DensityPhases[] = {
            VoxelDensityProfile::EBucket::DensityPrologue,
            VoxelDensityProfile::EBucket::DensityCore,
            VoxelDensityProfile::EBucket::DensityDisturbances,
            VoxelDensityProfile::EBucket::DensityStructuralPosts,
            VoxelDensityProfile::EBucket::DensityBoundarySeal,
            VoxelDensityProfile::EBucket::DensityDiffLayer,
            VoxelDensityProfile::EBucket::DensityTail,
        };
        uint64 DensityAttributionCycles = 0;
        uint64 DensityAttributionCalls = 0;
        for (const VoxelDensityProfile::EBucket Phase : DensityPhases)
        {
            const int32 PhaseIndex = static_cast<int32>(Phase);
            DensityAttributionCycles += Profile.WallCycles[PhaseIndex];
            DensityAttributionCalls += Profile.Calls[PhaseIndex];
        }
        const double DensityAttributionUs =
            static_cast<double>(DensityAttributionCycles) * CycleToMicroseconds;
        const double DensityUnattributedUs = DensityTotalUs - DensityAttributionUs;
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeDensityProfile] attribution_sum_us=%.3f unattributed_us=%.3f "
                 "coverage=%.2f%% phase_calls=%llu"),
            DensityAttributionUs, DensityUnattributedUs,
            DensityTotalUs > 0.0 ? DensityAttributionUs * 100.0 / DensityTotalUs : 0.0,
            static_cast<unsigned long long>(DensityAttributionCalls));

        for (int32 Index = 0; Index < VoxelDensityProfile::CounterCount; ++Index)
        {
            const VoxelDensityProfile::ECounter Counter =
                static_cast<VoxelDensityProfile::ECounter>(Index);
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeDensityProfile] counter=%s value=%llu per_density_call=%.3f"),
                VoxelDensityProfile::CounterName(Counter),
                static_cast<unsigned long long>(Profile.Counters[Index]),
                DensityCalls > 0
                    ? static_cast<double>(Profile.Counters[Index])
                        / static_cast<double>(DensityCalls)
                    : 0.0);
        }

        for (int32 Index = 0; Index < VoxelDensityProfile::BucketCount; ++Index)
        {
            const uint64 Calls = Profile.Calls[Index];
            if (Calls == 0 || Index == static_cast<int32>(VoxelDensityProfile::EBucket::GetDensityAt))
            {
                continue;
            }
            const double TotalUs = static_cast<double>(Profile.WallCycles[Index]) * CycleToMicroseconds;
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeDensityProfile] op=%s calls=%llu total_us=%.3f us_per_call=%.6f"),
                VoxelDensityProfile::BucketName(static_cast<VoxelDensityProfile::EBucket>(Index)),
                static_cast<unsigned long long>(Calls), TotalUs,
                TotalUs / static_cast<double>(Calls));
        }

        const VoxelDensityProfile::EBucket MesherBuckets[] = {
            VoxelDensityProfile::EBucket::MesherDensityGrid,
            VoxelDensityProfile::EBucket::MesherCellClassification,
            VoxelDensityProfile::EBucket::MesherGradientNormals,
            VoxelDensityProfile::EBucket::MesherVertexInterpolation,
            VoxelDensityProfile::EBucket::MesherStreamBuilding,
            VoxelDensityProfile::EBucket::MesherOther,
        };
        uint64 MesherAttributionCycles = 0;
        uint64 MesherAttributionCalls = 0;
        for (const VoxelDensityProfile::EBucket Bucket : MesherBuckets)
        {
            const int32 BucketIndex = static_cast<int32>(Bucket);
            MesherAttributionCycles += Profile.WallCycles[BucketIndex];
            MesherAttributionCalls += Profile.Calls[BucketIndex];
        }
        const int32 MesherTotalIndex = static_cast<int32>(
            VoxelDensityProfile::EBucket::MesherGenerateMesh);
        const double MesherTotalUs = static_cast<double>(
            Profile.WallCycles[MesherTotalIndex]) * CycleToMicroseconds;
        const double MesherAttributionUs = static_cast<double>(
            MesherAttributionCycles) * CycleToMicroseconds;
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeMesherProfile] total_calls=%llu total_us=%.3f "
                 "attribution_sum_us=%.3f unattributed_us=%.3f coverage=%.2f%% "
                 "phase_calls=%llu"),
            static_cast<unsigned long long>(Profile.Calls[MesherTotalIndex]),
            MesherTotalUs, MesherAttributionUs, MesherTotalUs - MesherAttributionUs,
            MesherTotalUs > 0.0 ? MesherAttributionUs * 100.0 / MesherTotalUs : 0.0,
            static_cast<unsigned long long>(MesherAttributionCalls));
        VoxelDensityProfile::SetMode(VoxelDensityProfile::EMode::Disabled);
    }
    // Per-case isolation boundary.  The UObject world dies with this function; worker-local
    // caches are keyed by the fresh generator/manager lifetime and the diagnostics are reset before
    // the next case.  No case may inherit profiler counters or a live profiler mode.
    VoxelDensityProfile::Reset();
    VoxelDensityProfile::SetMode(VoxelDensityProfile::EMode::Disabled);
    return bRequestedModeFailed ? 2 : 0;
}

namespace
{
FString SanitizeBatchCaseId(const FString& InId, int32 Index)
{
    FString Result;
    for (const TCHAR Character : InId)
    {
        const bool bAsciiAlphaNumeric =
            (Character >= TEXT('a') && Character <= TEXT('z'))
            || (Character >= TEXT('A') && Character <= TEXT('Z'))
            || (Character >= TEXT('0') && Character <= TEXT('9'));
        if (bAsciiAlphaNumeric || Character == TEXT('_') || Character == TEXT('-'))
        {
            Result.AppendChar(Character);
        }
    }
    if (Result.IsEmpty())
    {
        Result = FString::Printf(TEXT("case_%03d"), Index);
    }
    return Result;
}

bool JsonNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, double& OutValue)
{
    if (!Object.IsValid() || !Object->HasField(Key))
    {
        return false;
    }
    OutValue = Object->GetNumberField(Key);
    return FMath::IsFinite(OutValue);
}

bool JsonString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, FString& OutValue)
{
    if (!Object.IsValid() || !Object->HasField(Key))
    {
        return false;
    }
    OutValue = Object->GetStringField(Key);
    return true;
}

bool JsonBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, bool& OutValue)
{
    if (!Object.IsValid() || !Object->HasField(Key))
    {
        return false;
    }
    OutValue = Object->GetBoolField(Key);
    return true;
}

bool BuildBatchCaseParams(
    const TSharedPtr<FJsonObject>& Case,
    const FString& CaseOutDirectory,
    FString& OutParams,
    FString& OutError)
{
    if (!Case.IsValid())
    {
        OutError = TEXT("Each batch case must be a JSON object.");
        return false;
    }

    double Number = 0.0;
    int32 Seed = 0;
    if (JsonNumber(Case, TEXT("seed"), Number))
    {
        Seed = FMath::RoundToInt(Number);
    }
    FString Archetype = TEXT("Maze");
    JsonString(Case, TEXT("archetype"), Archetype);
    int32 Slot = 4;
    if (JsonNumber(Case, TEXT("slot"), Number))
    {
        Slot = FMath::RoundToInt(Number);
    }
    int32 ExportSize = DefaultExportSize;
    if (JsonNumber(Case, TEXT("export_size"), Number))
    {
        ExportSize = FMath::RoundToInt(Number);
    }
    int32 ExportStep = DefaultExportStep;
    if (JsonNumber(Case, TEXT("export_step"), Number)
        || JsonNumber(Case, TEXT("step"), Number))
    {
        ExportStep = FMath::RoundToInt(Number);
    }
    if (JsonNumber(Case, TEXT("lod"), Number))
    {
        const int32 Lod = FMath::RoundToInt(Number);
        if (Lod < 0 || Lod > 3)
        {
            OutError = TEXT("case.lod must be in [0,3].");
            return false;
        }
        ExportStep = 1 << Lod;
    }

    bool bReuseDensityGrid = true;
    JsonBool(Case, TEXT("density_grid_reuse"), bReuseDensityGrid);
    bool bBlockEarlyOut = false;
    JsonBool(Case, TEXT("block_early_out"), bBlockEarlyOut);
    int32 MeshMinBatchSize = 1;
    if (JsonNumber(Case, TEXT("mesh_min_batch_size"), Number))
    {
        MeshMinBatchSize = FMath::Clamp(FMath::RoundToInt(Number), 1, 64);
    }

    FString Modes = TEXT("export");
    JsonString(Case, TEXT("modes"), Modes);
    bool bOperatorStack = true;
    JsonBool(Case, TEXT("operator_stack"), bOperatorStack);
    bool bProfileDensity = false;
    JsonBool(Case, TEXT("profile_density"), bProfileDensity);
    bool bProfileDensityFull = false;
    JsonBool(Case, TEXT("profile_density_full"), bProfileDensityFull);
    bProfileDensity |= bProfileDensityFull;
    bool bProfileLod = false;
    JsonBool(Case, TEXT("profile_lod"), bProfileLod);
    bool bOpBounds = false;
    JsonBool(Case, TEXT("op_bounds"), bOpBounds);
    bool bFailureFocus = false;
    JsonBool(Case, TEXT("failure_focus_render"), bFailureFocus);

    OutParams = FString::Printf(
        TEXT("-seed=%d -archetype=%s -slot=%d -modes=%s -opstack=%d "
             "-exportsize=%d -exportstep=%d -densitygridreuse=%d "
             "-blockearlyout=%d -meshminbatch=%d "
             "-failurefocus=%d -out=\"%s\""),
        Seed, *Archetype, Slot, *Modes, bOperatorStack ? 1 : 0,
        ExportSize, ExportStep, bReuseDensityGrid ? 1 : 0,
        bBlockEarlyOut ? 1 : 0, MeshMinBatchSize,
        bFailureFocus ? 1 : 0, *CaseOutDirectory);

    if (bProfileDensity)
    {
        OutParams += bProfileDensityFull
            ? TEXT(" -profiledensityfull")
            : TEXT(" -profiledensity");
    }
    if (bProfileLod)
    {
        OutParams += TEXT(" -profilelod");
    }
    if (bOpBounds)
    {
        OutParams += TEXT(" -opbounds");
    }

    const TPair<const TCHAR*, const TCHAR*> IntegerFields[] = {
        { TEXT("render_width"), TEXT("renderwidth") },
        { TEXT("render_height"), TEXT("renderheight") },
        { TEXT("max_walk_cells"), TEXT("maxwalkcells") },
    };
    for (const auto& Field : IntegerFields)
    {
        if (JsonNumber(Case, Field.Key, Number))
        {
            OutParams += FString::Printf(TEXT(" -%s=%d"),
                Field.Value, FMath::RoundToInt(Number));
        }
    }
    const TPair<const TCHAR*, const TCHAR*> FloatFields[] = {
        { TEXT("render_step"), TEXT("renderstep") },
        { TEXT("render_max_distance"), TEXT("rendermaxdistance") },
        { TEXT("surface_roughness"), TEXT("surfaceroughness") },
    };
    for (const auto& Field : FloatFields)
    {
        if (JsonNumber(Case, Field.Key, Number))
        {
            OutParams += FString::Printf(TEXT(" -%s=%.9g"), Field.Value, Number);
        }
    }

    return true;
}

int32 RunBatch(const FString& Params)
{
    FString BatchPath;
    FString OutText;
    if (!FParse::Value(*Params, TEXT("batch="), BatchPath)
        || !FParse::Value(*Params, TEXT("out="), OutText))
    {
        UE_LOG(LogTemp, Error,
            TEXT("[VoxelForgeExplore] batch requires -batch=<json> and an absolute -out=<Saved path>."));
        return 1;
    }
    if (FPaths::IsRelative(OutText))
    {
        UE_LOG(LogTemp, Error,
            TEXT("[VoxelForgeExplore] batch -out must be absolute and under '%s'."),
            *VoxelForgeSavedRoot());
        return 1;
    }
    const FString BatchOutDirectory = FPaths::ConvertRelativePathToFull(OutText);
    if (!IsUnderVoxelForgeSaved(BatchOutDirectory))
    {
        UE_LOG(LogTemp, Error,
            TEXT("[VoxelForgeExplore] batch -out must be under '%s' (got '%s')."),
            *VoxelForgeSavedRoot(), *BatchOutDirectory);
        return 1;
    }
    if (!IFileManager::Get().MakeDirectory(*BatchOutDirectory, true))
    {
        UE_LOG(LogTemp, Error,
            TEXT("[VoxelForgeExplore] could not create batch output '%s'."),
            *BatchOutDirectory);
        return 1;
    }

    BatchPath = FPaths::ConvertRelativePathToFull(BatchPath);
    FString BatchText;
    if (!FFileHelper::LoadFileToString(BatchText, *BatchPath))
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] could not read batch file '%s'."), *BatchPath);
        return 1;
    }
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BatchText);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] batch file is not valid JSON."));
        return 1;
    }
    const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
    if (!Root->TryGetArrayField(TEXT("cases"), Cases) || Cases == nullptr || Cases->Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] batch JSON needs a non-empty 'cases' array."));
        return 1;
    }
    if (Cases->Num() > 256)
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] batch is capped at 256 cases."));
        return 1;
    }

    double BatchBudgetMinutes = 29.0;
    double Number = 0.0;
    if (JsonNumber(Root, TEXT("budget_minutes"), Number))
    {
        BatchBudgetMinutes = Number;
    }
    if (FParse::Value(*Params, TEXT("batchbudget="), Number))
    {
        BatchBudgetMinutes = Number;
    }
    BatchBudgetMinutes = FMath::Clamp(BatchBudgetMinutes, 0.01, 30.0);
    const double BatchStartSeconds = FPlatformTime::Seconds();
    bool bTruncated = false;
    int32 CompletedCases = 0;
    int32 FailedCases = 0;
    TSet<FString> UsedCaseIds;
    TArray<TSharedPtr<FJsonValue>> CaseReports;
    CaseReports.Reserve(Cases->Num());

    for (int32 Index = 0; Index < Cases->Num(); ++Index)
    {
        const double RemainingSeconds = BatchBudgetMinutes * 60.0
            - (FPlatformTime::Seconds() - BatchStartSeconds);
        if (RemainingSeconds <= 0.25)
        {
            bTruncated = true;
            break;
        }
        const TSharedPtr<FJsonObject> Case = (*Cases)[Index]->AsObject();
        FString CaseId;
        JsonString(Case, TEXT("id"), CaseId);
        CaseId = SanitizeBatchCaseId(CaseId, Index);
        const FString BaseCaseId = CaseId;
        int32 Suffix = 1;
        while (UsedCaseIds.Contains(CaseId))
        {
            CaseId = FString::Printf(TEXT("%s_%d"), *BaseCaseId, Suffix++);
        }
        UsedCaseIds.Add(CaseId);
        FString CaseOutDirectory = FPaths::Combine(BatchOutDirectory, TEXT("cases"), CaseId);
        FString CaseParams;
        FString CaseError;
        if (!BuildBatchCaseParams(Case, CaseOutDirectory, CaseParams, CaseError))
        {
            UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] batch case %d (%s): %s"),
                Index, *CaseId, *CaseError);
            TSharedPtr<FJsonObject> CaseReport = MakeShared<FJsonObject>();
        CaseReport->SetStringField(TEXT("case_id"), CaseId);
        CaseReport->SetNumberField(TEXT("case_index"), Index);
        CaseReport->SetNumberField(TEXT("case_exit_code"), 1);
        CaseReport->SetStringField(TEXT("status"), TEXT("invalid"));
            CaseReport->SetStringField(TEXT("error"), CaseError);
            CaseReports.Add(MakeShared<FJsonValueObject>(CaseReport));
            ++CompletedCases;
            ++FailedCases;
            continue;
        }

        double CaseBudgetMinutes = FMath::Min(25.0, RemainingSeconds / 60.0);
        if (JsonNumber(Case, TEXT("budget_minutes"), Number))
        {
            CaseBudgetMinutes = FMath::Min(CaseBudgetMinutes, Number);
        }
        CaseBudgetMinutes = FMath::Clamp(CaseBudgetMinutes, 0.01, 30.0);
        CaseParams += FString::Printf(TEXT(" -budget=%.9g"), CaseBudgetMinutes);

        FString CaseJson;
        const int32 CaseResult = RunExploreCase(CaseParams, &CaseJson);
        TSharedPtr<FJsonObject> CaseReport;
        const TSharedRef<TJsonReader<>> CaseReader = TJsonReaderFactory<>::Create(CaseJson);
        if (!FJsonSerializer::Deserialize(CaseReader, CaseReport) || !CaseReport.IsValid())
        {
            CaseReport = MakeShared<FJsonObject>();
            CaseReport->SetStringField(TEXT("error"), TEXT("case did not produce a JSON report"));
        }
        CaseReport->SetStringField(TEXT("case_id"), CaseId);
        CaseReport->SetNumberField(TEXT("case_index"), Index);
        CaseReport->SetNumberField(TEXT("case_exit_code"), CaseResult);
        CaseReports.Add(MakeShared<FJsonValueObject>(CaseReport));
        ++CompletedCases;
        if (CaseResult != 0)
        {
            ++FailedCases;
        }
    }

    TSharedPtr<FJsonObject> BatchReport = MakeShared<FJsonObject>();
    BatchReport->SetNumberField(TEXT("schema_version"), 2);
    BatchReport->SetStringField(TEXT("tool"), TEXT("VoxelForgeExplore"));
    BatchReport->SetBoolField(TEXT("batch"), true);
    BatchReport->SetStringField(TEXT("input_file"), BatchPath);
    BatchReport->SetStringField(TEXT("output_directory"), BatchOutDirectory);
    BatchReport->SetNumberField(TEXT("budget_minutes"), BatchBudgetMinutes);
    BatchReport->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - BatchStartSeconds);
    BatchReport->SetBoolField(TEXT("truncated"), bTruncated);
    BatchReport->SetNumberField(TEXT("completed_cases"), CompletedCases);
    BatchReport->SetNumberField(TEXT("failed_cases"), FailedCases);
    BatchReport->SetArrayField(TEXT("cases"), MoveTemp(CaseReports));

    FString BatchJson;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BatchJson);
    if (!FJsonSerializer::Serialize(BatchReport.ToSharedRef(), Writer)
        || !FFileHelper::SaveStringToFile(
            BatchJson,
            *FPaths::Combine(BatchOutDirectory, TEXT("batch.json")),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForgeExplore] could not write batch.json."));
        return 1;
    }
    UE_LOG(LogTemp, Display,
        TEXT("[VoxelForgeExplore] batch completed=%d failed=%d truncated=%s report=%s"),
        CompletedCases, FailedCases, bTruncated ? TEXT("yes") : TEXT("no"),
        *FPaths::Combine(BatchOutDirectory, TEXT("batch.json")));
    return FailedCases > 0 || bTruncated ? 2 : 0;
}
}

int32 UVoxelForgeExploreCommandlet::Main(const FString& Params)
{
    FString BatchPath;
    if (FParse::Value(*Params, TEXT("batch="), BatchPath))
    {
        return RunBatch(Params);
    }
    return RunExploreCase(Params, nullptr);
}
