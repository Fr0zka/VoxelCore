// Tier 4c — offline season composition, manifest determinism, and exact rebuild.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "VoxelDensityOpStack.h"
#include "VoxelSeasonManifest.h"
#include "VoxelStrateComposer.h"

namespace
{
    bool SameDensityBits(float A, float B)
    {
        return FMemory::Memcmp(&A, &B, sizeof(float)) == 0;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeComposerSeasonTest,
    "VoxelForge.Composer.Season",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeComposerSeasonTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FVoxelStrateCorpus Corpus;
    FString CorpusReport;
    TestTrue(TEXT("season test loads a valid corpus"),
             Corpus.LoadFromAssetRegistry(CorpusReport) && Corpus.IsValid());
    if (!Corpus.IsValid())
    {
        AddError(CorpusReport);
        return false;
    }

    FVoxelSeasonCompositionSettings Settings;
    Settings.Corpus = &Corpus;
    Settings.CandidateCount = 24;
    Settings.SelectedStrateCount = 6;
    Settings.StrateHeightInChunks = 4;
    Settings.InterStrateGapChunks = 0;
    Settings.OriginSpineRadius = 14.0f;
    Settings.WorldRadiusVoxels = 0.0f;
    Settings.OutputDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("VoxelForge"), TEXT("SeasonTest"));
    Settings.bWriteArtifacts = true;
    Settings.bWriteReviewPage = true;
    Settings.bIncludeCandidateAudit = true;

    const int32 SeasonSeed = 0x2468;
    const FVoxelSeasonManifest Manifest = VF_ComposeSeason(SeasonSeed, Settings);
    TestTrue(TEXT("composition produces a usable season manifest"), Manifest.IsUsable());
    if (!Manifest.IsUsable())
    {
        AddError(Manifest.Error);
        return false;
    }
    TestEqual(TEXT("candidate count is the configured number of attempts"),
              Manifest.CandidateCount, Settings.CandidateCount);
    TestEqual(TEXT("selected count is the configured spine length"),
              Manifest.SelectedCount, Settings.SelectedStrateCount);
    TestEqual(TEXT("candidate audit contains every attempt"),
              Manifest.CandidateAudit.Num(), Settings.CandidateCount);
    TestEqual(TEXT("WorldRadiusVoxels remains zero in the artifact"),
              Manifest.WorldRadiusVoxels, 0.0f);
    TestEqual(TEXT("selected grounded/outlier counts add to generated slots"),
              Manifest.GroundedSelectedCount + Manifest.OutlierSelectedCount,
              Manifest.SelectedCount - Manifest.FixedSelectedCount);

    for (int32 Index = 0; Index < Manifest.Strates.Num(); ++Index)
    {
        const FVoxelSeasonStrate& Strate = Manifest.Strates[Index];
        TestEqual(FString::Printf(TEXT("slot %d keeps descent order"), Index),
                  Strate.DepthIndex, Index);
        TestTrue(FString::Printf(TEXT("slot %d passed non-vacuous gate"), Index),
                 Strate.bPassedNonVacuous);
        TestTrue(FString::Printf(TEXT("slot %d passed largest-component gate"), Index),
                 Strate.bPassedLargestComponent);
        TestTrue(FString::Printf(TEXT("slot %d passed primordial law"), Index),
                 Strate.bPassedPrimordialLaw
                     && Strate.PrimordialLawResult == EVoxelConnectivityResult::Connected);
        TestTrue(FString::Printf(TEXT("slot %d has zero world radius"), Index),
                 Manifest.WorldRadiusVoxels == 0.0f);
    }

    TestTrue(TEXT("season manifest JSON was emitted"), !Manifest.SerializedJson.IsEmpty());
    TestTrue(TEXT("season manifest file was emitted"), FPaths::FileExists(Manifest.ManifestPath));
    TestTrue(TEXT("selected-season review page was emitted"),
             FPaths::FileExists(Manifest.ReviewPagePath));
    FString ReviewHtml;
    if (FFileHelper::LoadFileToString(ReviewHtml, *Manifest.ReviewPagePath))
    {
        TestTrue(TEXT("review page explains why each selected slot was chosen"),
                 ReviewHtml.Contains(TEXT("Why selected:")));
        TestTrue(TEXT("review page marks boss slots"), ReviewHtml.Contains(TEXT("Boss slot:")));
    }
    else
    {
        AddError(FString::Printf(TEXT("could not read review page %s"), *Manifest.ReviewPagePath));
    }

    // Compose again with the same inputs. SerializedJson excludes derived filesystem paths, so
    // this is the byte-level determinism assertion for the portable artifact itself.
    const FVoxelSeasonManifest RepeatManifest = VF_ComposeSeason(SeasonSeed, Settings);
    TestTrue(TEXT("repeat composition is usable"), RepeatManifest.IsUsable());
    TestEqual(TEXT("same season seed produces byte-identical JSON"),
              RepeatManifest.SerializedJson, Manifest.SerializedJson);

    FVoxelSeasonManifest LoadedManifest;
    FString LoadReport;
    TestTrue(TEXT("manifest loads from its JSON file"),
             VF_LoadVoxelSeasonManifest(Manifest.ManifestPath, LoadedManifest, LoadReport));
    if (!LoadedManifest.IsUsable())
    {
        AddError(LoadReport);
        return false;
    }
    TestEqual(TEXT("loaded manifest has the same number of selected strates"),
              LoadedManifest.Strates.Num(), Manifest.Strates.Num());

    int64 ComparedDensitySamples = 0;
    bool bAllDensityBitsIdentical = true;
    for (int32 Index = 0; Index < Manifest.Strates.Num(); ++Index)
    {
        const FVoxelSeasonStrate& OriginalStrate = Manifest.Strates[Index];
        const FVoxelSeasonStrate& LoadedStrate = LoadedManifest.Strates[Index];
        TestTrue(FString::Printf(TEXT("slot %d parameter vector survives JSON bit-identically"), Index),
                 VF_AreStrateArchetypeParamsBitIdentical(
                     OriginalStrate.Params, LoadedStrate.Params, OriginalStrate.Archetype));
        TestTrue(FString::Printf(TEXT("slot %d recipe survives JSON identically"), Index),
                 VF_AreStrateStructureRecipesIdentical(
                     OriginalStrate.Recipe, LoadedStrate.Recipe));

        FVoxelOpStack OriginalStack;
        FVoxelOpContext OriginalContext;
        FVoxelOpStack LoadedStack;
        FVoxelOpContext LoadedContext;
        FString BuildError;
        TestTrue(FString::Printf(TEXT("slot %d original stack rebuilds"), Index),
                 VF_RebuildVoxelSeasonStrate(
                     OriginalStrate, Manifest.OriginSpineRadius, nullptr,
                     OriginalStack, OriginalContext, &BuildError));
        TestTrue(FString::Printf(TEXT("slot %d loaded stack rebuilds"), Index),
                 VF_RebuildVoxelSeasonStrate(
                     LoadedStrate, LoadedManifest.OriginSpineRadius, nullptr,
                     LoadedStack, LoadedContext, &BuildError));
        if (OriginalStack.Num() == 0 || LoadedStack.Num() == 0)
        {
            bAllDensityBitsIdentical = false;
            continue;
        }
        TestEqual(FString::Printf(TEXT("slot %d original/rebuilt stack operation count"), Index),
                  LoadedStack.Num(), OriginalStack.Num());
        TestEqual(FString::Printf(TEXT("slot %d original context radius"), Index),
                  OriginalContext.WorldRadiusVoxels, 0.0f);
        TestEqual(FString::Printf(TEXT("slot %d loaded context radius"), Index),
                  LoadedContext.WorldRadiusVoxels, 0.0f);
        OriginalStack.PrepareChunk(OriginalContext);
        LoadedStack.PrepareChunk(LoadedContext);

        // Compare the complete bounded probe lattice for each selected field at two-voxel
        // spacing, not a handful of favorable points. Every sampled scalar must retain its
        // exact IEEE-754 bits after JSON load and stack reconstruction.
        constexpr int32 ProbeHalfExtent = 32;
        constexpr int32 ProbeStep = 2;
        for (int32 Z = OriginalStrate.BottomWorldZ;
             Z < OriginalStrate.TopWorldZ && bAllDensityBitsIdentical;
             Z += ProbeStep)
        {
            for (int32 Y = -ProbeHalfExtent;
                 Y < ProbeHalfExtent && bAllDensityBitsIdentical;
                 Y += ProbeStep)
            {
                for (int32 X = -ProbeHalfExtent;
                     X < ProbeHalfExtent;
                     X += ProbeStep)
                {
                    const float OriginalDensity = OriginalStack.EvalMC(
                        static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                    const float LoadedDensity = LoadedStack.EvalMC(
                        static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                    ++ComparedDensitySamples;
                    if (!SameDensityBits(OriginalDensity, LoadedDensity))
                    {
                        bAllDensityBitsIdentical = false;
                        AddError(FString::Printf(
                            TEXT("slot %d density mismatch at (%d,%d,%d): 0x%08x vs 0x%08x"),
                            Index, X, Y, Z,
                            *reinterpret_cast<const uint32*>(&OriginalDensity),
                            *reinterpret_cast<const uint32*>(&LoadedDensity)));
                        break;
                    }
                }
            }
        }
    }
    AddInfo(FString::Printf(TEXT("round-trip density probe compared %lld bit-identical samples"),
                            ComparedDensitySamples));
    TestTrue(TEXT("manifest round-trip density field is bit-identical"),
             bAllDensityBitsIdentical);
    return true;
}

#endif
