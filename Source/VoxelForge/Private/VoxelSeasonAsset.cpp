#include "VoxelSeasonAsset.h"

#include "VoxelSeasonManifest.h"
#include "VoxelDensityOpStack.h"

#if WITH_EDITOR
#include "Misc/FileHelper.h"
#endif

namespace
{
    bool ValidateRuntimeRecipes(const FVoxelSeasonManifest& Manifest, FString& OutReport)
    {
        for (const FVoxelSeasonStrate& Strate : Manifest.Strates)
        {
            if (!Strate.bUsesRecipe) continue;
            FVoxelOpStack ValidationStack;
            FVoxelOpContext ValidationContext;
            FString BuildError;
            if (!VF_BuildSeasonStrateStack(
                    Strate, Manifest.OriginSpineRadius, nullptr,
                    ValidationStack, ValidationContext, &BuildError))
            {
                OutReport = FString::Printf(
                    TEXT("season slot %d recipe is not materialisable: %s"),
                    Strate.DepthIndex, *BuildError);
                return false;
            }
        }
        return true;
    }
}

FPrimaryAssetId UVoxelSeasonAsset::GetPrimaryAssetId() const
{
    return FPrimaryAssetId(TEXT("VoxelSeason"), GetFName());
}

bool UVoxelSeasonAsset::LoadManifest(FVoxelSeasonManifest& OutManifest, FString& OutReport) const
{
    if (ManifestJson.IsEmpty())
    {
        OutReport = TEXT("season asset contains no imported manifest JSON");
        OutManifest = FVoxelSeasonManifest();
        return false;
    }

    if (!VF_DeserializeVoxelSeasonManifest(ManifestJson, OutManifest, OutReport))
    {
        return false;
    }
    if (!OutManifest.ContentHash.Equals(ManifestContentHash, ESearchCase::IgnoreCase)
        || OutManifest.SchemaVersion != SchemaVersion
        || OutManifest.Season != SeasonNumber
        || OutManifest.Seed != SeasonSeed
        || FMemory::Memcmp(&OutManifest.OriginSpineRadius,
                           &OriginSpineRadius, sizeof(float)) != 0
        || FMemory::Memcmp(&OutManifest.WorldRadiusVoxels,
                           &WorldRadiusVoxels, sizeof(float)) != 0)
    {
        OutReport = TEXT("season asset metadata does not match its embedded manifest; re-import the JSON");
        OutManifest.bValid = false;
        OutManifest.Error = OutReport;
        return false;
    }
    if (!ValidateRuntimeRecipes(OutManifest, OutReport))
    {
        OutManifest.bValid = false;
        OutManifest.Error = OutReport;
        return false;
    }
    return true;
}

bool UVoxelSeasonAsset::SetManifestJson(const FString& InManifestJson, FString& OutReport)
{
    FVoxelSeasonManifest Parsed;
    if (!VF_DeserializeVoxelSeasonManifest(InManifestJson, Parsed, OutReport))
    {
        return false;
    }
    if (!ValidateRuntimeRecipes(Parsed, OutReport))
    {
        return false;
    }

    Modify();
    ManifestJson = InManifestJson;
    SchemaVersion = Parsed.SchemaVersion;
    SeasonNumber = Parsed.Season;
    SeasonSeed = Parsed.Seed;
    OriginSpineRadius = Parsed.OriginSpineRadius;
    WorldRadiusVoxels = Parsed.WorldRadiusVoxels;
    ManifestContentHash = Parsed.ContentHash;
    ReferencedDefinitions.Reset();
    TSet<FString> SeenDefinitionPaths;
    for (const FVoxelSeasonStrate& Strate : Parsed.Strates)
    {
        if (!Strate.SourceDefinitionPath.IsEmpty()
            && !SeenDefinitionPaths.Contains(Strate.SourceDefinitionPath))
        {
            SeenDefinitionPaths.Add(Strate.SourceDefinitionPath);
            ReferencedDefinitions.Add(TSoftObjectPtr<UVoxelStrateDefinition>(
                FSoftObjectPath(Strate.SourceDefinitionPath)));
        }
    }
    MarkPackageDirty();
    OutReport = FString::Printf(
        TEXT("imported season %d seed %d (%d strates, hash %s)"),
        SeasonNumber, SeasonSeed, Parsed.Strates.Num(), *ManifestContentHash);
    return true;
}

void UVoxelSeasonAsset::ImportSeasonManifestJson()
{
#if WITH_EDITOR
    FString JsonText;
    FString Report;
    if (SourceManifestJson.FilePath.IsEmpty()
        || !FFileHelper::LoadFileToString(JsonText, *SourceManifestJson.FilePath))
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelSeasonAsset] Could not read manifest JSON '%s'."),
               *SourceManifestJson.FilePath);
        return;
    }
    if (!SetManifestJson(JsonText, Report))
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelSeasonAsset] Import failed: %s"), *Report);
        return;
    }
    UE_LOG(LogTemp, Log, TEXT("[VoxelSeasonAsset] %s"), *Report);
#else
    UE_LOG(LogTemp, Error, TEXT("[VoxelSeasonAsset] JSON import is editor-only."));
#endif
}
