// Cookable runtime carrier for one reviewed season manifest.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelSeasonAsset.generated.h"

struct FVoxelSeasonManifest;
class UVoxelStrateDefinition;

/**
 * The JSON remains the reviewed/diffable source artifact.  This asset embeds that exact text so
 * Unreal's cooker can carry it into a packaged build, together with an independently stored digest
 * used to detect an accidentally stale or edited asset.
 */
UCLASS(BlueprintType)
class VOXELFORGE_API UVoxelSeasonAsset : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    virtual FPrimaryAssetId GetPrimaryAssetId() const override;

    /** Parse and validate the embedded manifest, including both content-hash copies. */
    bool LoadManifest(FVoxelSeasonManifest& OutManifest, FString& OutReport) const;

    /** Runtime-safe import core used by the editor button and automation. */
    bool SetManifestJson(const FString& InManifestJson, FString& OutReport);

    int32 GetSeasonNumber() const { return SeasonNumber; }
    int32 GetSeasonSeed() const { return SeasonSeed; }
    float GetOriginSpineRadius() const { return OriginSpineRadius; }
    float GetWorldRadiusVoxels() const { return WorldRadiusVoxels; }

    /** Create the asset in Content Browser, choose a JSON file, then press this button. */
    UFUNCTION(CallInEditor, Category = "Season")
    void ImportSeasonManifestJson();

#if WITH_EDITORONLY_DATA
    UPROPERTY(EditAnywhere, Category = "Season|Import", meta = (FilePathFilter = "json"))
    FFilePath SourceManifestJson;
#endif

private:
    UPROPERTY(VisibleAnywhere, Category = "Season")
    int32 SchemaVersion = 0;

    UPROPERTY(VisibleAnywhere, Category = "Season")
    int32 SeasonNumber = 0;

    UPROPERTY(VisibleAnywhere, Category = "Season")
    int32 SeasonSeed = 0;

    UPROPERTY(VisibleAnywhere, Category = "Season")
    float OriginSpineRadius = 0.0f;

    UPROPERTY(VisibleAnywhere, Category = "Season")
    float WorldRadiusVoxels = 0.0f;

    UPROPERTY(VisibleAnywhere, Category = "Season")
    FString ManifestContentHash;

    UPROPERTY(VisibleAnywhere, Category = "Season", meta = (MultiLine = true))
    FString ManifestJson;

    // Import-derived cook references for fixed/authored content bags named by the JSON. Generated
    // strates legitimately have no source definition and use deterministic C++ defaults at runtime.
    UPROPERTY(VisibleAnywhere, Category = "Season")
    TArray<TSoftObjectPtr<UVoxelStrateDefinition>> ReferencedDefinitions;
};
