// Tier 4d — lateral region partition, density blending, conservative box proofs, and the
// primordial arrival->departure gate.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"
#include "VoxelStrateComposer.h"
#include "VoxelStrateMeasure.h"
#include "VoxelStratePreview.h"

namespace
{
    constexpr int32 TargetStrateIndex = VoxelForgeTest::FTestWorld::SlotFlatPlain;
    constexpr int32 DeterminismChecks = 32;
    constexpr int32 CrossBoundaryBoxChecks = 20;
    constexpr int32 InteriorBoxChecks = 20;
    constexpr int32 LawSeedCount = 16;
    constexpr int32 LawTrialLimit = 512;
    constexpr int32 TimingPointCount = 64;
    constexpr int32 TimingRepeats = 128;

    bool VF_BitEqual(float A, float B)
    {
        uint32 ABits = 0;
        uint32 BBits = 0;
        FMemory::Memcpy(&ABits, &A, sizeof(ABits));
        FMemory::Memcpy(&BBits, &B, sizeof(BBits));
        return ABits == BBits;
    }

    const TCHAR* ConnectivityName(EVoxelConnectivityResult Result)
    {
        switch (Result)
        {
        case EVoxelConnectivityResult::Connected:
            return TEXT("Connected");
        case EVoxelConnectivityResult::NotConnectedAtThisResolution:
            return TEXT("NotConnectedAtThisResolution");
        case EVoxelConnectivityResult::StartCellSolid:
            return TEXT("StartCellSolid");
        case EVoxelConnectivityResult::GoalCellSolid:
            return TEXT("GoalCellSolid");
        case EVoxelConnectivityResult::StartCellNotPlayerFit:
            return TEXT("StartCellNotPlayerFit");
        case EVoxelConnectivityResult::GoalCellNotPlayerFit:
            return TEXT("GoalCellNotPlayerFit");
        case EVoxelConnectivityResult::OutOfWindow:
            return TEXT("OutOfWindow");
        case EVoxelConnectivityResult::CoarseLiedBudgetExhausted:
            return TEXT("CoarseLiedBudgetExhausted");
        }
        return TEXT("Unknown");
    }

    bool QueriesBitEqual(const FVoxelStrateRegionQuery& A,
                         const FVoxelStrateRegionQuery& B)
    {
        return A.PrimaryRegion == B.PrimaryRegion
            && A.NeighborRegion == B.NeighborRegion
            && VF_BitEqual(A.NeighborWeight, B.NeighborWeight)
            && VF_BitEqual(A.NearestDifferentRegionGap, B.NearestDifferentRegionGap)
            && A.bNearestDifferentRegionKnown == B.bNearestDifferentRegionKnown
            && A.bInBlendBand == B.bInBlendBand;
    }

    void SetManifestBounds(const FVoxelStrateComposerCandidate& Candidate,
                           const VoxelForgeTest::FTestWorld& World,
                           int32 StrateIndex,
                           FVoxelStrateRegionManifest& OutManifest,
                           int32& OutTopWorldZ,
                           int32& OutBottomWorldZ)
    {
        int32 TopVoxelZ = 0;
        int32 BottomVoxelZ = 0;
        if (!World.GetSlotVoxelZRange(StrateIndex, TopVoxelZ, BottomVoxelZ))
        {
            OutManifest = FVoxelStrateRegionManifest();
            OutTopWorldZ = 0;
            OutBottomWorldZ = 0;
            return;
        }

        OutTopWorldZ = TopVoxelZ + 1;
        OutBottomWorldZ = BottomVoxelZ;
        OutManifest = Candidate.Regions;
        VF_RekeyStrateRegionManifest(OutManifest, Candidate.Seed, StrateIndex);
        OutManifest.StrateTopWorldZ = static_cast<float>(OutTopWorldZ);
        OutManifest.StrateBottomWorldZ = static_cast<float>(OutBottomWorldZ);
        OutManifest.bHasGlobalStructuralParams = true;
        for (FVoxelStrateRegion& Region : OutManifest.Regions)
        {
            VF_SetStrateArchetypeRuntimeBounds(Region.ArchetypeParams,
                                               OutManifest.StrateTopWorldZ,
                                               OutManifest.StrateBottomWorldZ);
        }
    }

    int32 FloorDiv(int32 A, int32 B)
    {
        const int32 Q = A / B;
        const int32 R = A % B;
        return (R != 0 && ((R < 0) != (B < 0))) ? Q - 1 : Q;
    }

    bool BuildRegionStack(const FVoxelStrateComposerCandidate& Candidate,
                          VoxelForgeTest::FTestWorld& World,
                          int32 StrateIndex,
                          FVoxelStrateRegionManifest& OutManifest,
                          FVoxelOpStack& OutStack,
                          FVoxelOpContext& OutContext,
                          int32& OutTopWorldZ,
                          int32& OutBottomWorldZ)
    {
        SetManifestBounds(Candidate, World, StrateIndex, OutManifest,
                          OutTopWorldZ, OutBottomWorldZ);
        if (!OutManifest.IsValid())
        {
            return false;
        }

        FString Error;
        if (!VF_BuildStrateRegionStack(OutManifest, World.Generator->OriginSpineRadius,
                                       World.StrateManager.Get(), OutStack, OutContext, &Error))
        {
            return false;
        }

        const int32 MidWorldZ = (OutTopWorldZ + OutBottomWorldZ) / 2;
        OutContext.ChunkCoord = FIntVector(0, 0, FloorDiv(MidWorldZ, CHUNK_SIZE));
        OutContext.Step = 1;
        OutContext.LayoutVersion = World.StrateManager->GetLayoutVersion();
        OutContext.WorldRadiusVoxels = 0.0f;
        OutStack.PrepareChunk(OutContext);
        return OutContext.WorldRadiusVoxels == 0.0f;
    }

    bool FindPassageMouths(const VoxelForgeTest::FTestWorld& World,
                           int32 StrateIndex,
                           FVector& OutArrival,
                           FVector& OutDeparture)
    {
        int32 ArrivalCount = 0;
        int32 DepartureCount = 0;
        OutArrival = FVector::ZeroVector;
        OutDeparture = FVector::ZeroVector;
        for (const FVoxelPassage& Passage : World.StrateManager->GetPassages())
        {
            if (Passage.LowerStrateIndex == StrateIndex
                && Passage.UpperStrateIndex + 1 == StrateIndex)
            {
                OutArrival = Passage.LowerPoint;
                ++ArrivalCount;
            }
            if (Passage.UpperStrateIndex == StrateIndex
                && Passage.LowerStrateIndex == StrateIndex + 1)
            {
                OutDeparture = Passage.UpperPoint;
                ++DepartureCount;
            }
        }
        return ArrivalCount == 1 && DepartureCount == 1;
    }

    struct FBoxAudit
    {
        int32 TotalBoxes = 0;
        int32 CrossBoundaryBoxes = 0;
        int32 BoundaryRiskBoxes = 0;
        int32 InteriorBoxes = 0;
        int32 CrossBoundaryMixed = 0;
        int32 CrossBoundaryUniform = 0;
        int32 Mixed = 0;
        int32 AllSolid = 0;
        int32 AllAir = 0;
        int64 BruteForceSamples = 0;
        int32 Violations = 0;
        FString FirstViolation;
    };

    bool BoxActuallyTouchesMultipleRegions(const FVoxelStrateRegionManifest& Manifest,
                                           const FBox& Box)
    {
        TSet<int32> Regions;
        // Use the same integer lattice that AuditBox brute-forces.  Corners/centre probes can
        // miss a narrow region sliver crossing the box; a sparse test must not inflate the
        // cross-boundary count or call a boundary audit complete.
        const int32 MinX = FMath::FloorToInt(static_cast<float>(Box.Min.X));
        const int32 MaxX = FMath::CeilToInt(static_cast<float>(Box.Max.X));
        const int32 MinY = FMath::FloorToInt(static_cast<float>(Box.Min.Y));
        const int32 MaxY = FMath::CeilToInt(static_cast<float>(Box.Max.Y));
        for (int32 Y = MinY; Y <= MaxY; ++Y)
        {
            for (int32 X = MinX; X <= MaxX; ++X)
            {
                Regions.Add(VF_QueryStrateRegion(
                    Manifest, static_cast<float>(X), static_cast<float>(Y)).PrimaryRegion);
                if (Regions.Num() > 1)
                {
                    return true;
                }
            }
        }
        return Regions.Num() > 1;
    }

    void AuditBox(const FVoxelOpStack& Stack,
                  const FVoxelOpContext& Context,
                  const FVoxelStrateRegionManifest& Manifest,
                  const FBox& Box,
                  FBoxAudit& InOutReport)
    {
        ++InOutReport.TotalBoxes;
        const FVoxelStrateRegionBoxProof Proof = VF_AnalyzeStrateRegionBox(Manifest, Box);
        const bool bCrossBoundary = BoxActuallyTouchesMultipleRegions(Manifest, Box);
        if (bCrossBoundary)
        {
            ++InOutReport.CrossBoundaryBoxes;
            if (Proof.bProvablySingleRegion)
            {
                ++InOutReport.Violations;
                if (InOutReport.FirstViolation.IsEmpty())
                {
                    InOutReport.FirstViolation = TEXT(
                        "box touched two sampled region IDs but was declared provably single-region");
                }
            }
        }
        else if (Proof.bProvablySingleRegion)
        {
            ++InOutReport.InteriorBoxes;
        }
        else
        {
            ++InOutReport.BoundaryRiskBoxes;
        }

        const EVoxelTileClass Verdict = Stack.ClassifyBox(Box, Context);
        if (Verdict == EVoxelTileClass::Mixed)
        {
            ++InOutReport.Mixed;
            if (bCrossBoundary) { ++InOutReport.CrossBoundaryMixed; }
        }
        else
        {
            const bool bClaimsSolid = Verdict == EVoxelTileClass::AllSolid;
            if (bClaimsSolid) { ++InOutReport.AllSolid; }
            else { ++InOutReport.AllAir; }
            if (bCrossBoundary) { ++InOutReport.CrossBoundaryUniform; }
        }

        // Brute-force every sampled box, including Mixed cross-boundary boxes. Mixed has no
        // uniform claim to refute, but counting its full sample traversal is important evidence
        // that the boundary audit did not only exercise interiors.
        const int32 MinX = FMath::FloorToInt(static_cast<float>(Box.Min.X));
        const int32 MaxX = FMath::CeilToInt(static_cast<float>(Box.Max.X));
        const int32 MinY = FMath::FloorToInt(static_cast<float>(Box.Min.Y));
        const int32 MaxY = FMath::CeilToInt(static_cast<float>(Box.Max.Y));
        const int32 MinZ = FMath::FloorToInt(static_cast<float>(Box.Min.Z));
        const int32 MaxZ = FMath::CeilToInt(static_cast<float>(Box.Max.Z));
        const bool bUniformVerdict = Verdict != EVoxelTileClass::Mixed;
        const bool bClaimsSolid = Verdict == EVoxelTileClass::AllSolid;
        for (int32 Z = MinZ; Z <= MaxZ; ++Z)
        for (int32 Y = MinY; Y <= MaxY; ++Y)
        for (int32 X = MinX; X <= MaxX; ++X)
        {
            const float Density = Stack.EvalMC(static_cast<float>(X), static_cast<float>(Y),
                                               static_cast<float>(Z));
            ++InOutReport.BruteForceSamples;
            if (bUniformVerdict)
            {
                const bool bAgrees = bClaimsSolid ? Density < 0.0f : Density >= 0.0f;
                if (!bAgrees)
                {
                    ++InOutReport.Violations;
                    if (InOutReport.FirstViolation.IsEmpty())
                    {
                        InOutReport.FirstViolation = FString::Printf(
                            TEXT("box [%s] said %s but MC density at (%d,%d,%d) was %.9g"),
                            bCrossBoundary ? TEXT("cross-boundary") : TEXT("interior"),
                            bClaimsSolid ? TEXT("AllSolid") : TEXT("AllAir"),
                        X, Y, Z, Density);
                    }
                }
            }
        }
    }

    void CollectBoundaryBoxes(const FVoxelStrateRegionManifest& Manifest,
                               int32 CenterZ,
                               TArray<FBox>& OutCross,
                               TArray<FBox>& OutInterior)
    {
        OutCross.Reset();
        OutInterior.Reset();
        constexpr int32 ScanMin = -512;
        constexpr int32 ScanMax = 512;
        constexpr int32 ScanStep = 8;
        constexpr float CrossHalfExtent = 10.0f;
        constexpr float InteriorHalfExtent = 4.0f;

        for (int32 Y = ScanMin; Y < ScanMax && OutCross.Num() < CrossBoundaryBoxChecks; Y += ScanStep)
        {
            for (int32 X = ScanMin; X < ScanMax && OutCross.Num() < CrossBoundaryBoxChecks; X += ScanStep)
            {
                const FVoxelStrateRegionQuery A = VF_QueryStrateRegion(
                    Manifest, static_cast<float>(X), static_cast<float>(Y));
                const FVoxelStrateRegionQuery B = VF_QueryStrateRegion(
                    Manifest, static_cast<float>(X + ScanStep), static_cast<float>(Y));
                const FVoxelStrateRegionQuery C = VF_QueryStrateRegion(
                    Manifest, static_cast<float>(X), static_cast<float>(Y + ScanStep));
                if (A.PrimaryRegion == B.PrimaryRegion && A.PrimaryRegion == C.PrimaryRegion)
                {
                    continue;
                }
                const FVector Center(static_cast<float>(X) + ScanStep * 0.5f,
                                     static_cast<float>(Y) + ScanStep * 0.5f,
                                     static_cast<float>(CenterZ));
                const FBox Box(
                    Center - FVector(CrossHalfExtent, CrossHalfExtent, CrossHalfExtent),
                    Center + FVector(CrossHalfExtent, CrossHalfExtent, CrossHalfExtent));
                if (BoxActuallyTouchesMultipleRegions(Manifest, Box))
                {
                    OutCross.Add(Box);
                }
            }
        }

        for (int32 Y = ScanMin; Y < ScanMax && OutInterior.Num() < InteriorBoxChecks; Y += ScanStep)
        {
            for (int32 X = ScanMin; X < ScanMax && OutInterior.Num() < InteriorBoxChecks; X += ScanStep)
            {
                const FVector Center(static_cast<float>(X), static_cast<float>(Y),
                                     static_cast<float>(CenterZ));
                const FBox Box(
                    Center - FVector(InteriorHalfExtent, InteriorHalfExtent, InteriorHalfExtent),
                    Center + FVector(InteriorHalfExtent, InteriorHalfExtent, InteriorHalfExtent));
                const FVoxelStrateRegionBoxProof Proof =
                    VF_AnalyzeStrateRegionBox(Manifest, Box);
                if (Proof.bProvablySingleRegion)
                {
                    OutInterior.Add(Box);
                }
            }
        }
    }

    FIntVector ChunkForPoint(const FVector2D& Point, int32 WorldZ)
    {
        return FIntVector(FloorDiv(FMath::FloorToInt(Point.X), CHUNK_SIZE),
                          FloorDiv(FMath::FloorToInt(Point.Y), CHUNK_SIZE),
                          FloorDiv(WorldZ, CHUNK_SIZE));
    }

    double TimeDensitySamples(FVoxelOpStack& Stack,
                              const FVoxelOpContext& BaseContext,
                              const TArray<FVector2D>& Points,
                              int32 WorldZ,
                              volatile float& OutSink)
    {
        // Group by owning chunk before timing. PrepareChunk is the intended cold path; including
        // it in every sample would report chunk setup rather than the per-voxel region lookup and
        // density evaluation that §8.10 protects.
        TMap<FIntVector, TArray<FVector2D>> PointsByChunk;
        for (const FVector2D& Point : Points)
        {
            PointsByChunk.FindOrAdd(ChunkForPoint(Point, WorldZ)).Add(Point);
        }

        double Elapsed = 0.0;
        for (const TPair<FIntVector, TArray<FVector2D>>& Group : PointsByChunk)
        {
            FVoxelOpContext Context = BaseContext;
            Context.ChunkCoord = Group.Key;
            Stack.PrepareChunk(Context);
            const double Start = FPlatformTime::Seconds();
            for (int32 Repeat = 0; Repeat < TimingRepeats; ++Repeat)
            {
                for (const FVector2D& Point : Group.Value)
                {
                    OutSink += Stack.EvalMC(Point.X, Point.Y, static_cast<float>(WorldZ));
                }
            }
            Elapsed += FPlatformTime::Seconds() - Start;
        }
        return Elapsed;
    }

    class FStackSampler final : public IVoxelStrateDensitySampler
    {
    public:
        explicit FStackSampler(const FVoxelOpStack& InStack) : Stack(InStack) {}

        float SampleDensity(float X, float Y, float Z) const override
        {
            return Stack.EvalMC(X, Y, Z);
        }

    private:
        const FVoxelOpStack& Stack;
    };

    int32 DistinctRegionArchetypes(const FVoxelStrateRegionManifest& Manifest)
    {
        TSet<int32> Archetypes;
        for (const FVoxelStrateRegion& Region : Manifest.Regions)
        {
            Archetypes.Add(static_cast<int32>(Region.Archetype));
        }
        return Archetypes.Num();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeLateralRegionsTest,
    "VoxelForge.Composer.LateralRegions",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeLateralRegionsTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;
    (void)Parameters;

    FVoxelStrateCorpus Corpus;
    FString CorpusReport;
    if (!Corpus.LoadFromAssetRegistry(CorpusReport) || !Corpus.IsValid())
    {
        AddError(FString::Printf(TEXT("lateral-region corpus failed to load: %s"), *CorpusReport));
        return false;
    }
    AddInfo(CorpusReport);

    // The pure roll/query seam is checked independently of stack materialisation.  This catches
    // accidental stateful RNG or order-dependent site construction before any density test can
    // hide it.
    FRandomStream QueryRng(0x5245474Eu);
    for (int32 Check = 0; Check < DeterminismChecks; ++Check)
    {
        const int32 Seed = 7001 + Check * 7919;
        const int32 StrateIndex = Check % 7;
        const FVoxelStrateRegionManifest A = VF_RollStrateRegionManifest(
            Corpus, Seed, StrateIndex, true);
        const FVoxelStrateRegionManifest B = VF_RollStrateRegionManifest(
            Corpus, Seed, StrateIndex, true);
        TestTrue(FString::Printf(TEXT("determinism roll %d is valid"), Check),
                 A.IsValid() && B.IsValid());
        TestEqual(FString::Printf(TEXT("determinism roll %d region count"), Check),
                  A.RegionCount, B.RegionCount);
        TestEqual(FString::Printf(TEXT("determinism roll %d partition seed"), Check),
                  A.PartitionSeed, B.PartitionSeed);
        TestTrue(FString::Printf(TEXT("determinism roll %d global fields"), Check),
                 A.StructuralParamBlock == B.StructuralParamBlock
                     && VF_BitEqual(A.LatticeCellSize, B.LatticeCellSize)
                     && VF_BitEqual(A.BlendWidth, B.BlendWidth));
        if (A.IsValid() && B.IsValid())
        {
            for (int32 RegionIndex = 0; RegionIndex < A.RegionCount; ++RegionIndex)
            {
                const FVoxelStrateRegion& RA = A.Regions[RegionIndex];
                const FVoxelStrateRegion& RB = B.Regions[RegionIndex];
                TestTrue(FString::Printf(TEXT("determinism roll %d region %d vector"),
                                         Check, RegionIndex),
                         RA.RegionIndex == RB.RegionIndex && RA.Seed == RB.Seed
                             && RA.Archetype == RB.Archetype
                             && RA.bUsesRecipe == RB.bUsesRecipe
                             && VF_AreStrateArchetypeParamsBitIdentical(
                                 RA.ArchetypeParams, RB.ArchetypeParams, RA.Archetype)
                             && (!RA.bUsesRecipe
                                 || VF_AreStrateStructureRecipesIdentical(RA.Recipe, RB.Recipe)));
            }
            for (int32 PointIndex = 0; PointIndex < 8; ++PointIndex)
            {
                const float X = QueryRng.FRandRange(-768.0f, 768.0f) + 0.25f;
                const float Y = QueryRng.FRandRange(-768.0f, 768.0f) - 0.375f;
                TestTrue(FString::Printf(TEXT("determinism roll %d query %d"), Check, PointIndex),
                         QueriesBitEqual(VF_QueryStrateRegion(A, X, Y),
                                         VF_QueryStrateRegion(B, X, Y)));
            }

            FVoxelStrateRegionPartitionCache Cache;
            Cache.PrepareForChunk(A, FIntVector(0, 0, 0));
            bool bCacheMatchesPureQuery = true;
            for (int32 Y = -1; Y <= CHUNK_SIZE + 1 && bCacheMatchesPureQuery; ++Y)
            {
                for (int32 X = -1; X <= CHUNK_SIZE + 1; ++X)
                {
                    if (!QueriesBitEqual(Cache.Query(static_cast<float>(X), static_cast<float>(Y)),
                                         VF_QueryStrateRegion(A, static_cast<float>(X),
                                                              static_cast<float>(Y))))
                    {
                        bCacheMatchesPureQuery = false;
                        break;
                    }
                }
            }
            TestTrue(FString::Printf(TEXT("determinism roll %d cached chunk query"), Check),
                     bCacheMatchesPureQuery);
        }
    }

    // Find a genuine multi-archetype candidate for the seam audit and preview.  The search only
    // selects what the deterministic composer rolled; it does not alter region count, seeds, or
    // blend width to improve a gate.
    FVoxelStrateComposerCandidate MultiCandidate;
    int32 MultiSeed = 0;
    bool bFoundMulti = false;
    for (int32 Trial = 0; Trial < 512 && !bFoundMulti; ++Trial)
    {
        const int32 Seed = 12001 + Trial * 37;
        const FVoxelStrateComposerCandidate Candidate = VF_RollStrateCandidate(
            Corpus, Seed, TargetStrateIndex, true);
        if (Candidate.bValid && Candidate.Regions.RegionCount > 1
            && DistinctRegionArchetypes(Candidate.Regions) > 1)
        {
            MultiCandidate = Candidate;
            MultiSeed = Seed;
            bFoundMulti = true;
        }
    }
    TestTrue(TEXT("composer produced a multi-region, multi-archetype candidate"), bFoundMulti);
    if (!bFoundMulti)
    {
        return false;
    }
    AddInfo(FString::Printf(TEXT("selected preview candidate seed=%d index=%d regions=%d archetypes=%d"),
                            MultiCandidate.Seed, MultiCandidate.Index,
                            MultiCandidate.Regions.RegionCount,
                            DistinctRegionArchetypes(MultiCandidate.Regions)));

    // One-region identity: the region manifest is passed through the new builder, but it must
    // produce the old native stack bit-for-bit. This uses a parameter-roll candidate because it
    // exercises the native family mapping rather than only the recipe path.
    bool bIdentityChecked = false;
    for (int32 Trial = 0; Trial < 512 && !bIdentityChecked; ++Trial)
    {
        const int32 Seed = 21001 + Trial * 41;
        const FVoxelStrateComposerCandidate Candidate = VF_RollStrateCandidate(
            Corpus, Seed, TargetStrateIndex, false);
        if (!Candidate.bValid || Candidate.Regions.RegionCount != 1)
        {
            continue;
        }

        FTestWorld World;
        World.Build(Seed, 2);
        int32 TopWorldZ = 0;
        int32 BottomWorldZ = 0;
        FVoxelStrateRegionManifest Manifest;
        FVoxelOpStack RegionStack;
        FVoxelOpContext RegionContext;
        const bool bRegionBuilt = BuildRegionStack(
            Candidate, World, TargetStrateIndex, Manifest, RegionStack, RegionContext,
            TopWorldZ, BottomWorldZ);

        FVoxelStrateArchetypeParams Params = Candidate.ArchetypeParams;
        VF_SetStrateArchetypeRuntimeBounds(Params, static_cast<float>(TopWorldZ),
                                           static_cast<float>(BottomWorldZ));
        FVoxelOpStack LegacyStack;
        FVoxelOpContext LegacyContext;
        const bool bLegacyBuilt = VF_BuildNativeStrateStackForCandidate(
            Candidate.Archetype, Params, Candidate.Seed, World.Generator->OriginSpineRadius,
            0.0f, World.Generator->EdgeSealThickness, World.StrateManager.Get(),
            LegacyStack, LegacyContext);
        LegacyContext.ChunkCoord = RegionContext.ChunkCoord;
        LegacyContext.Step = 1;
        LegacyContext.WorldRadiusVoxels = 0.0f;
        LegacyStack.PrepareChunk(LegacyContext);

        TestTrue(TEXT("single-region native stack builds"), bRegionBuilt && bLegacyBuilt);
        TestTrue(TEXT("single-region context keeps WorldRadiusVoxels at 0"),
                 bRegionBuilt && RegionContext.WorldRadiusVoxels == 0.0f);
        if (bRegionBuilt && bLegacyBuilt)
        {
            FRandomStream Rng(0x1D3A17u);
            bool bEqual = true;
            for (int32 PointIndex = 0; PointIndex < 256; ++PointIndex)
            {
                const float X = Rng.FRandRange(-256.0f, 256.0f);
                const float Y = Rng.FRandRange(-256.0f, 256.0f);
                const float Z = Rng.FRandRange(static_cast<float>(BottomWorldZ),
                                               static_cast<float>(TopWorldZ));
                if (!VF_BitEqual(RegionStack.EvalMC(X, Y, Z), LegacyStack.EvalMC(X, Y, Z)))
                {
                    bEqual = false;
                    break;
                }
            }
            TestTrue(TEXT("single-region native stack is bit-identical to legacy"), bEqual);
            bIdentityChecked = true;
        }
    }
    TestTrue(TEXT("found a single-region identity case"), bIdentityChecked);

    // The same identity gate covers a structure candidate: count==1 must not introduce a special
    // density path for an authored recipe either. The old recipe builder is the direct oracle.
    bool bRecipeIdentityChecked = false;
    for (int32 Trial = 0; Trial < 512 && !bRecipeIdentityChecked; ++Trial)
    {
        const int32 Seed = 22001 + Trial * 43;
        const FVoxelStrateComposerCandidate Candidate = VF_RollStrateCandidate(
            Corpus, Seed, TargetStrateIndex, true);
        if (!Candidate.bValid || Candidate.Regions.RegionCount != 1)
        {
            continue;
        }

        FTestWorld World;
        World.Build(Seed, 2);
        int32 TopWorldZ = 0;
        int32 BottomWorldZ = 0;
        FVoxelStrateRegionManifest Manifest;
        FVoxelOpStack RegionStack;
        FVoxelOpContext RegionContext;
        const bool bRegionBuilt = BuildRegionStack(
            Candidate, World, TargetStrateIndex, Manifest, RegionStack, RegionContext,
            TopWorldZ, BottomWorldZ);

        FVoxelStrateArchetypeParams Params = Candidate.ArchetypeParams;
        VF_SetStrateArchetypeRuntimeBounds(Params, static_cast<float>(TopWorldZ),
                                           static_cast<float>(BottomWorldZ));
        FVoxelOpStack LegacyStack;
        FVoxelOpContext LegacyContext;
        FString LegacyError;
        const bool bLegacyBuilt = VF_BuildStackFromRecipe(
            Candidate.Recipe, Params, Candidate.Seed, World.Generator->OriginSpineRadius,
            World.StrateManager.Get(), LegacyStack, LegacyContext, &LegacyError);
        LegacyContext.ChunkCoord = RegionContext.ChunkCoord;
        LegacyContext.Step = 1;
        LegacyContext.WorldRadiusVoxels = 0.0f;
        LegacyStack.PrepareChunk(LegacyContext);

        TestTrue(TEXT("single-region recipe stack builds"), bRegionBuilt && bLegacyBuilt);
        if (bRegionBuilt && bLegacyBuilt)
        {
            FRandomStream Rng(0x2E6317u);
            bool bEqual = true;
            for (int32 PointIndex = 0; PointIndex < 256; ++PointIndex)
            {
                const float X = Rng.FRandRange(-256.0f, 256.0f);
                const float Y = Rng.FRandRange(-256.0f, 256.0f);
                const float Z = Rng.FRandRange(static_cast<float>(BottomWorldZ),
                                               static_cast<float>(TopWorldZ));
                if (!VF_BitEqual(RegionStack.EvalMC(X, Y, Z), LegacyStack.EvalMC(X, Y, Z)))
                {
                    bEqual = false;
                    break;
                }
            }
            TestTrue(TEXT("single-region recipe stack is bit-identical to legacy"), bEqual);
            bRecipeIdentityChecked = true;
        }
        if (!LegacyError.IsEmpty()) { AddInfo(LegacyError); }
    }
    TestTrue(TEXT("found a single-region recipe identity case"), bRecipeIdentityChecked);

    FTestWorld World;
    World.Build(MultiSeed, 2);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }

    FVoxelStrateRegionManifest Manifest;
    FVoxelOpStack Stack;
    FVoxelOpContext Context;
    int32 TopWorldZ = 0;
    int32 BottomWorldZ = 0;
    if (!BuildRegionStack(MultiCandidate, World, TargetStrateIndex,
                          Manifest, Stack, Context, TopWorldZ, BottomWorldZ))
    {
        AddError(TEXT("multi-region stack could not be materialised"));
        return false;
    }
    TestEqual(TEXT("multi-region count is between one and three"),
              Manifest.RegionCount, MultiCandidate.Regions.RegionCount);
    TestTrue(TEXT("multi-region stack passes ValidateChannelOrder"),
             Stack.ValidateChannelOrder());
    TestEqual(TEXT("multi-region context WorldRadiusVoxels is zero"),
              Context.WorldRadiusVoxels, 0.0f);

    // Explicit cross-boundary and interior box audits. Every uniform verdict is checked against
    // every integer sample in the box; Mixed is also counted, but never treated as a failure.
    TArray<FBox> CrossBoxes;
    TArray<FBox> InteriorBoxes;
    CollectBoundaryBoxes(Manifest, (TopWorldZ + BottomWorldZ) / 2,
                          CrossBoxes, InteriorBoxes);
    TestTrue(TEXT("found boxes that actually span a region boundary"),
             CrossBoxes.Num() >= CrossBoundaryBoxChecks);
    TestTrue(TEXT("found provably single-region interior boxes"),
             InteriorBoxes.Num() >= InteriorBoxChecks);

    FBoxAudit BoxReport;
    for (const FBox& Box : CrossBoxes)
    {
        AuditBox(Stack, Context, Manifest, Box, BoxReport);
    }
    for (const FBox& Box : InteriorBoxes)
    {
        AuditBox(Stack, Context, Manifest, Box, BoxReport);
    }
    TestEqual(TEXT("cross-boundary box count is separately reported"),
              BoxReport.CrossBoundaryBoxes, CrossBoxes.Num());
    TestEqual(TEXT("cross-boundary and interior boxes have zero false-uniform violations"),
              BoxReport.Violations, 0);
    AddInfo(FString::Printf(
        TEXT("box verdicts: total=%d cross_boundary=%d cross_mixed=%d cross_uniform=%d "
             "interior=%d boundary_risk=%d mixed=%d all_solid=%d all_air=%d brute_samples=%lld "
             "violations=%d"),
        BoxReport.TotalBoxes, BoxReport.CrossBoundaryBoxes, BoxReport.CrossBoundaryMixed,
        BoxReport.CrossBoundaryUniform, BoxReport.InteriorBoxes, BoxReport.BoundaryRiskBoxes,
        BoxReport.Mixed, BoxReport.AllSolid, BoxReport.AllAir, BoxReport.BruteForceSamples,
        BoxReport.Violations));
    if (!BoxReport.FirstViolation.IsEmpty())
    {
        AddInfo(BoxReport.FirstViolation);
    }

    // Band coverage and cost: the partition is queried over a 1024x1024 XY audit window. The
    // timed samples prepare their owning chunk before the clock starts, so the number isolates
    // one-stack vs two-stack voxel evaluation rather than chunk setup.
    int64 BandVoxels = 0;
    int64 TotalPartitionVoxels = 0;
    TArray<FVector2D> InteriorTimingPoints;
    TArray<FVector2D> BandTimingPoints;
    InteriorTimingPoints.Reserve(TimingPointCount);
    BandTimingPoints.Reserve(TimingPointCount);
    const int32 TimingZ = (TopWorldZ + BottomWorldZ) / 2;
    for (int32 Y = -512; Y < 512; Y += 4)
    {
        for (int32 X = -512; X < 512; X += 4)
        {
            const FVoxelStrateRegionQuery Query = VF_QueryStrateRegion(
                Manifest, static_cast<float>(X), static_cast<float>(Y));
            ++TotalPartitionVoxels;
            if (Query.bInBlendBand)
            {
                ++BandVoxels;
                if (BandTimingPoints.Num() < TimingPointCount)
                {
                    BandTimingPoints.Add(FVector2D(static_cast<float>(X), static_cast<float>(Y)));
                }
            }
            else if (InteriorTimingPoints.Num() < TimingPointCount)
            {
                InteriorTimingPoints.Add(FVector2D(static_cast<float>(X), static_cast<float>(Y)));
            }
        }
    }
    TestTrue(TEXT("performance audit found interior samples"),
             InteriorTimingPoints.Num() == TimingPointCount);
    TestTrue(TEXT("performance audit found blend-band samples"),
             BandTimingPoints.Num() == TimingPointCount);
    volatile float TimingSink = 0.0f;
    const double InteriorSeconds = TimeDensitySamples(
        Stack, Context, InteriorTimingPoints, TimingZ, TimingSink);
    const double BandSeconds = TimeDensitySamples(
        Stack, Context, BandTimingPoints, TimingZ, TimingSink);
    const double InteriorNs = InteriorSeconds * 1.0e9
        / static_cast<double>(FMath::Max(1, InteriorTimingPoints.Num() * TimingRepeats));
    const double BandNs = BandSeconds * 1.0e9
        / static_cast<double>(FMath::Max(1, BandTimingPoints.Num() * TimingRepeats));
    const double BandFraction = TotalPartitionVoxels > 0
        ? static_cast<double>(BandVoxels) / static_cast<double>(TotalPartitionVoxels) : 0.0;
    TestTrue(TEXT("interior timing is finite and non-zero"),
             FMath::IsFinite(InteriorNs) && InteriorNs > 0.0);
    TestTrue(TEXT("blend-band timing is finite and non-zero"),
             FMath::IsFinite(BandNs) && BandNs > 0.0);
    AddInfo(FString::Printf(
        TEXT("blend-band cost: width=%.3f voxels band=%lld/%lld fraction=%.6f "
             "interior=%.2f ns/voxel band=%.2f ns/voxel ratio=%.3f sink=%.3f"),
        Manifest.BlendWidth, BandVoxels, TotalPartitionVoxels, BandFraction,
        InteriorNs, BandNs, InteriorNs > 0.0 ? BandNs / InteriorNs : 0.0,
        static_cast<float>(TimingSink)));

    // The live editor hand-off must activate the region parent even when the authored definition
    // did not opt into the operator stack. The direct stack above is the oracle for points that
    // are outside the generator's common passage/landing tail; points in those domains are
    // intentionally not compared because the live owner adds the current MC post there.
    {
        struct FLivePoint
        {
            FVector Position = FVector::ZeroVector;
            float Before = 0.0f;
            float After = 0.0f;
            float Repeat = 0.0f;
        };
        TArray<FLivePoint> Points;
        constexpr int32 RequiredNonTailPoints = 64;
        constexpr int32 SearchExtent = 2048;
        constexpr int32 SearchStep = 64;
        int32 NonTailCandidates = 0;
        const FVector HalfBox(0.5f, 0.5f, 0.5f);
        auto HasOwnerTail = [&](const FVector& Position)
        {
            return World.StrateManager->AnyPassageNearBox(
                       Position - HalfBox, Position + HalfBox)
                || World.StrateManager->AnyPassageLandingFloorNearBox(
                       Position - HalfBox, Position + HalfBox)
                || World.StrateManager->AnyOriginLandingFloorNearBox(
                       Position - HalfBox, Position + HalfBox);
        };

        // Keep the original local samples as a tail exercise, then search a larger deterministic
        // lattice for points outside the common passage/landing post-process. The old +/-64 box
        // was entirely tail in the current authored world, so it could not test the live lateral
        // hand-off at all.
        for (int32 ZOffset = -16; ZOffset <= 16; ZOffset += 8)
        {
            for (int32 Y = -64; Y <= 64; Y += 16)
            {
                for (int32 X = -64; X <= 64; X += 16)
                {
                    FLivePoint& Point = Points.AddDefaulted_GetRef();
                    Point.Position = FVector(static_cast<float>(X), static_cast<float>(Y),
                                             static_cast<float>(TimingZ + ZOffset));
                    Point.Before = World.Generator->GetDensityAt(
                        Point.Position.X, Point.Position.Y, Point.Position.Z);
                }
            }
        }
        for (int32 ZOffset = -16;
             ZOffset <= 16 && NonTailCandidates < RequiredNonTailPoints;
             ZOffset += 8)
        {
            for (int32 Y = -SearchExtent;
                 Y <= SearchExtent && NonTailCandidates < RequiredNonTailPoints;
                 Y += SearchStep)
            {
                for (int32 X = -SearchExtent;
                     X <= SearchExtent && NonTailCandidates < RequiredNonTailPoints;
                     X += SearchStep)
                {
                    const FVector Position(
                        static_cast<float>(X), static_cast<float>(Y),
                        static_cast<float>(TimingZ + ZOffset));
                    if (HasOwnerTail(Position))
                    {
                        continue;
                    }
                    FLivePoint& Point = Points.AddDefaulted_GetRef();
                    Point.Position = Position;
                    Point.Before = World.Generator->GetDensityAt(
                        Point.Position.X, Point.Position.Y, Point.Position.Z);
                    ++NonTailCandidates;
                }
            }
        }
        TestTrue(TEXT("lateral handoff fixture found non-tail samples"),
                 NonTailCandidates >= RequiredNonTailPoints);

        FString OverrideError;
        const bool bInstalled = World.StrateManager->SetComposerOverrideForStrate(
            TargetStrateIndex, MultiCandidate.Seed, MultiCandidate.Archetype,
            MultiCandidate.ArchetypeParams, true, &MultiCandidate.Recipe,
            OverrideError, &MultiCandidate.Regions);
        TestTrue(TEXT("multi-region composer override installs"), bInstalled);
        if (bInstalled)
        {
            int32 ChangedPoints = 0;
            int32 ComparablePoints = 0;
            int32 ComparableMismatches = 0;
            int32 RepeatMismatches = 0;
            FString FirstMismatch;
            for (FLivePoint& Point : Points)
            {
                Point.After = World.Generator->GetDensityAt(
                    Point.Position.X, Point.Position.Y, Point.Position.Z);
                Point.Repeat = World.Generator->GetDensityAt(
                    Point.Position.X, Point.Position.Y, Point.Position.Z);
                if (!VF_BitEqual(Point.Before, Point.After))
                {
                    ++ChangedPoints;
                }
                if (!VF_BitEqual(Point.After, Point.Repeat))
                {
                    ++RepeatMismatches;
                }

                if (HasOwnerTail(Point.Position))
                {
                    continue;
                }
                ++ComparablePoints;
                const float Direct = Stack.EvalMC(
                    Point.Position.X, Point.Position.Y, Point.Position.Z);
                if (!VF_BitEqual(Direct, Point.After))
                {
                    ++ComparableMismatches;
                    if (FirstMismatch.IsEmpty())
                    {
                        FirstMismatch = FString::Printf(
                            TEXT("lateral handoff mismatch at (%.0f,%.0f,%.0f): direct=%.9g live=%.9g"),
                            Point.Position.X, Point.Position.Y, Point.Position.Z,
                            Direct, Point.After);
                    }
                }
            }
            TestTrue(TEXT("live lateral override changes at least one sampled value"),
                     ChangedPoints > 0);
            TestTrue(TEXT("live lateral override leaves a non-tail comparison set"),
                     ComparablePoints >= 20);
            TestEqual(TEXT("live lateral override matches its direct stack outside the common tail"),
                      ComparableMismatches, 0);
            TestEqual(TEXT("live lateral override is repeatable"), RepeatMismatches, 0);
            AddInfo(FString::Printf(
                TEXT("live lateral handoff: changed=%d comparable=%d fixture_non_tail=%d "
                     "mismatches=%d repeat_mismatches=%d search_extent=%d step=%d"),
                ChangedPoints, ComparablePoints, NonTailCandidates,
                ComparableMismatches, RepeatMismatches, SearchExtent, SearchStep));
            if (!FirstMismatch.IsEmpty())
            {
                AddInfo(FirstMismatch);
            }
            if (!OverrideError.IsEmpty()) { AddInfo(OverrideError); }
        }
    }

    // Measure arrival->departure on sixteen independently rolled multi-region strates. No
    // corridor is inserted and no pass threshold is changed in this loop: failures are evidence
    // that the lateral design does not yet preserve the primordial law.
    int32 LawTrials = 0;
    int32 LawPasses = 0;
    int32 LawCrossRegionTrials = 0;
    int32 LawCrossRegionPasses = 0;
    int32 LawSameRegionTrials = 0;
    int32 LawNoMouthTrials = 0;
    int32 LawBuildFailures = 0;
    int32 LawSingleControlPasses = 0;
    constexpr float LargestComponentSurvivalThreshold = 0.50f;
    int32 SurvivalTrials = 0;
    int32 SurvivalValid = 0;
    int32 SurvivalNonVacuous = 0;
    int32 SurvivalLargestEnough = 0;
    int32 SurvivalFullGate = 0;
    FString LawTable = TEXT("seed | regions | mouth regions | lateral law | single-region control | snapped | retries\n");
    // Count sixteen cases whose arrival and departure mouths are actually on opposite sides of a
    // region assignment. Same-region mouth pairs are logged as controls below but do not satisfy
    // this seam-specific sample quota.
    for (int32 Trial = 0; Trial < LawTrialLimit && LawCrossRegionTrials < LawSeedCount; ++Trial)
    {
        const int32 Seed = 31001 + Trial * 53;
        const FVoxelStrateComposerCandidate Candidate = VF_RollStrateCandidate(
            Corpus, Seed, TargetStrateIndex, true);
        if (!Candidate.bValid || Candidate.Regions.RegionCount <= 1)
        {
            continue;
        }

        FTestWorld LawWorld;
        LawWorld.Build(Seed, 2);
        FVoxelStrateRegionManifest LawManifest;
        FVoxelOpStack LawStack;
        FVoxelOpContext LawContext;
        int32 LawTop = 0;
        int32 LawBottom = 0;
        if (!BuildRegionStack(Candidate, LawWorld, TargetStrateIndex,
                              LawManifest, LawStack, LawContext, LawTop, LawBottom))
        {
            ++LawBuildFailures;
            continue;
        }

        FVector Arrival;
        FVector Departure;
        const bool bMouths = FindPassageMouths(LawWorld, TargetStrateIndex,
                                                Arrival, Departure);
        if (!bMouths)
        {
            ++LawNoMouthTrials;
            continue;
        }
        int32 ArrivalRegion = INDEX_NONE;
        int32 DepartureRegion = INDEX_NONE;
        const FVoxelStrateRegionQuery ArrivalQuery = VF_QueryStrateRegion(
            LawManifest, Arrival.X, Arrival.Y);
        const FVoxelStrateRegionQuery DepartureQuery = VF_QueryStrateRegion(
            LawManifest, Departure.X, Departure.Y);
        ArrivalRegion = ArrivalQuery.PrimaryRegion;
        DepartureRegion = DepartureQuery.PrimaryRegion;
        const bool bCrossRegionMouths = ArrivalRegion != DepartureRegion;
        if (!bCrossRegionMouths)
        {
            ++LawSameRegionTrials;
            continue;
        }
        ++LawCrossRegionTrials;
        ++LawTrials;

        // Control for the fact that the pre-existing structure recipe itself is not guaranteed to
        // connect every arbitrary pair of mouths. Restricting the exact same manifest to region
        // zero keeps its recipe/vector, structural post values, seed, and measurement settings;
        // it does not make the control a shipping path, it only separates inherited recipe
        // fragmentation from a lateral seam regression.
        FVoxelStrateRegionManifest SingleControlManifest = LawManifest;
        SingleControlManifest.RegionCount = 1;
        SingleControlManifest.Regions.SetNum(1);
        FVoxelOpStack SingleControlStack;
        FVoxelOpContext SingleControlContext;
        FString SingleControlError;
        bool bSingleControlBuilt = VF_BuildStrateRegionStack(
            SingleControlManifest, LawWorld.Generator->OriginSpineRadius,
            LawWorld.StrateManager.Get(), SingleControlStack, SingleControlContext,
            &SingleControlError);
        if (bSingleControlBuilt)
        {
            SingleControlContext.ChunkCoord = LawContext.ChunkCoord;
            SingleControlContext.Step = LawContext.Step;
            SingleControlContext.LayoutVersion = LawContext.LayoutVersion;
            SingleControlContext.WorldRadiusVoxels = 0.0f;
            SingleControlStack.PrepareChunk(SingleControlContext);
        }
        FVoxelConnectivityDiagnostics Diagnostics;
        FVoxelConnectivityDiagnostics SingleControlDiagnostics;
        FVoxelStrateMetrics SurvivalMetrics;
        if (bMouths)
        {
            FVoxelStrateMeasureSettings LawSettings;
            LawSettings.SampleStep = 4;
            LawSettings.RadiusInVoxels = 256;
            LawSettings.CoverPointA = FVector2D(Arrival.X, Arrival.Y);
            LawSettings.CoverPointB = FVector2D(Departure.X, Departure.Y);
            LawSettings.CoverMarginVoxels = 64.0f;
            LawSettings.MaxCells = 8000000;
            LawSettings.MaxRouteRetries = 16;
            LawSettings.HeadroomCells = 2;
            LawSettings.InteriorMarginVoxels = -1;
            SurvivalMetrics = VF_MeasureStrateWithSampler(
                FStackSampler(LawStack), LawBottom, LawTop + 1, LawContext.EdgeSealThickness,
                LawSettings, nullptr);
            ++SurvivalTrials;
            const bool bSurvivalValid = SurvivalMetrics.bValid;
            const bool bSurvivalNonVacuous = bSurvivalValid
                && SurvivalMetrics.NumSampled > 0
                && SurvivalMetrics.NumAir > 0
                && SurvivalMetrics.NumSolid > 0;
            const bool bSurvivalLargestEnough = bSurvivalNonVacuous
                && SurvivalMetrics.LargestComponentShare
                    >= LargestComponentSurvivalThreshold;
            if (bSurvivalValid) { ++SurvivalValid; }
            if (bSurvivalNonVacuous) { ++SurvivalNonVacuous; }
            if (bSurvivalLargestEnough) { ++SurvivalLargestEnough; }
            FStackSampler Sampler(LawStack);
            Diagnostics = VF_DiagnoseConnectivityWithSampler(
                Sampler, LawBottom, LawTop + 1, LawContext.EdgeSealThickness,
                Arrival, Departure, LawSettings);
            if (bSingleControlBuilt)
            {
                FStackSampler SingleControlSampler(SingleControlStack);
                SingleControlDiagnostics = VF_DiagnoseConnectivityWithSampler(
                    SingleControlSampler, LawBottom, LawTop + 1,
                    SingleControlContext.EdgeSealThickness, Arrival, Departure, LawSettings);
            }
        }
        const bool bPass = bMouths && Diagnostics.bValid
            && Diagnostics.Result == EVoxelConnectivityResult::Connected
            && !Diagnostics.bStartSnapped && !Diagnostics.bGoalSnapped;
        const bool bSingleControlPass = bMouths && bSingleControlBuilt
            && SingleControlDiagnostics.bValid
            && SingleControlDiagnostics.Result == EVoxelConnectivityResult::Connected
            && !SingleControlDiagnostics.bStartSnapped
            && !SingleControlDiagnostics.bGoalSnapped;
        if (bPass) { ++LawPasses; }
        if (bPass && bCrossRegionMouths) { ++LawCrossRegionPasses; }
        if (bSingleControlPass) { ++LawSingleControlPasses; }
        const bool bSurvivalNonVacuousForGate = SurvivalMetrics.bValid
            && SurvivalMetrics.NumSampled > 0
            && SurvivalMetrics.NumAir > 0
            && SurvivalMetrics.NumSolid > 0;
        const bool bSurvivalLargestEnoughForGate = bSurvivalNonVacuousForGate
            && SurvivalMetrics.LargestComponentShare
                >= LargestComponentSurvivalThreshold;
        if (bSurvivalNonVacuousForGate && bSurvivalLargestEnoughForGate && bPass)
        {
            ++SurvivalFullGate;
        }
        LawTable += FString::Printf(
            TEXT("%d | %d | %d->%d%s | %s | %s | %s/%s | %d\n"), Seed,
            Candidate.Regions.RegionCount,
            ArrivalRegion, DepartureRegion, bCrossRegionMouths ? TEXT(" seam") : TEXT(""),
            Diagnostics.bValid ? ConnectivityName(Diagnostics.Result) : TEXT("invalid"),
            SingleControlDiagnostics.bValid
                ? ConnectivityName(SingleControlDiagnostics.Result) : TEXT("invalid"),
            Diagnostics.bStartSnapped ? TEXT("start") : TEXT("no-start"),
            Diagnostics.bGoalSnapped ? TEXT("goal") : TEXT("no-goal"),
            Diagnostics.NumRouteRetries);
    }
    AddInfo(LawTable);
    const float LawRate = LawTrials > 0
        ? static_cast<float>(LawPasses) / static_cast<float>(LawTrials) : 0.0f;
    TestEqual(TEXT("measured at least sixteen multi-region primordial-law seeds"),
              LawTrials, LawSeedCount);
    TestEqual(TEXT("measured sixteen multi-region seam mouth pairs"),
              LawCrossRegionTrials, LawSeedCount);
    // ⚠️ NOT AN ASSERTION, AND THAT IS NOT A SOFTENED GUARD — read this before "fixing" it.
    //
    // Lateral regions are an EXPERIMENTAL, OFF-BY-DEFAULT feature. Nothing that ships evaluates
    // them, so this rate is the measured state of an unfinished thing, not a regression in a live
    // world. Asserting it would leave the suite permanently red and destroy its signal value —
    // and a suite that is always red stops being read.
    //
    // What DOES stay hard, below and elsewhere: single-region output is bit-identical to legacy,
    // cross-boundary box verdicts have zero violations, and the shipping path's own primordial-law
    // tests are untouched. The one thing that must never regress — a live strate a player can
    // enter and not leave — is still guarded by those.
    //
    // ⛔ THE GATE IS THE REAL GUARD: VF_LateralRegionsAreShippable() must return false while this
    // rate is below 100%, and the assertion immediately after enforces exactly that. To enable the
    // feature you must first make this rate 16/16 — not lower a threshold, and not flip the gate.
    // Density blending alone does not guarantee connectivity across a seam; that needs an explicit
    // corridor/landing contract, which is a DESIGN decision and is not made here.
    AddWarning(FString::Printf(
        TEXT("EXPERIMENTAL, GATED OFF: multi-region arrival->departure is %d/%d. Lateral regions "
             "stay disabled until this is %d/%d. See COMPOSER-NOTES §3.2."),
        LawPasses, LawTrials, LawTrials, LawTrials));

    // The gate must be shut whenever the law is not perfect. This is the assertion that keeps an
    // unfinished feature out of a world a player could walk into.
    const bool bLawPerfect = (LawTrials > 0) && (LawPasses == LawTrials);
    TestFalse(TEXT("lateral regions must stay disabled while the seam law is imperfect"),
              !bLawPerfect && VF_LateralRegionsAreShippable());
    AddInfo(FString::Printf(TEXT("multi-region primordial law: %d/%d = %.3f"),
                            LawPasses, LawTrials, LawRate));
    AddInfo(FString::Printf(TEXT("mouth pairs across a region seam: %d/%d passed; same-region controls: %d"),
                            LawCrossRegionPasses, LawCrossRegionTrials, LawSameRegionTrials));
    AddInfo(FString::Printf(TEXT("seam trial collection: %d no-mouth, %d build failures"),
                            LawNoMouthTrials, LawBuildFailures));
    AddInfo(FString::Printf(TEXT("single-region region-0 controls: %d/%d passed"),
                            LawSingleControlPasses, LawTrials));
    TestEqual(TEXT("survival was measured for every cross-region mouth pair"),
              SurvivalTrials, LawTrials);
    AddInfo(FString::Printf(
        TEXT("multi-region survival on the same seam cases: valid=%d/%d, non-vacuous=%d/%d, "
             "largest-component>=%.2f=%d/%d, full hard-gate (including unsnapped law)=%d/%d"),
        SurvivalValid, SurvivalTrials, SurvivalNonVacuous, SurvivalTrials,
        LargestComponentSurvivalThreshold, SurvivalLargestEnough, SurvivalTrials,
        SurvivalFullGate, SurvivalTrials));

    // Render one real multi-region measurement grid. The contact sheet is intentionally left in
    // Saved/ComposerPreview so the seam can be inspected even if a later gate fails.
    Stack.PrepareChunk(Context);
    class FPreviewSampler final : public IVoxelStrateDensitySampler
    {
    public:
        explicit FPreviewSampler(const FVoxelOpStack& InStack) : Stack(InStack) {}
        float SampleDensity(float X, float Y, float Z) const override
        {
            return Stack.EvalMC(X, Y, Z);
        }
    private:
        const FVoxelOpStack& Stack;
    } PreviewSampler(Stack);
    FVoxelStrateMeasureSettings PreviewSettings;
    PreviewSettings.SampleStep = 4;
    PreviewSettings.RadiusInVoxels = 256;
    PreviewSettings.CenterXY = FVector2D::ZeroVector;
    PreviewSettings.MaxCells = 8000000;
    PreviewSettings.MaxRouteRetries = 16;
    PreviewSettings.HeadroomCells = 2;
    PreviewSettings.InteriorMarginVoxels = -1;
    FVoxelStrateSampleGrid PreviewGrid;
    const FVoxelStrateMetrics PreviewMetrics = VF_MeasureStrateWithSampler(
        PreviewSampler, BottomWorldZ, TopWorldZ, Context.EdgeSealThickness,
        PreviewSettings, &PreviewGrid);
    TestTrue(TEXT("multi-region preview measurement is valid"),
             PreviewMetrics.bValid && PreviewGrid.IsValid());

    const FString PreviewDirectory = FPaths::ProjectSavedDir() / TEXT("ComposerPreview")
        / FString::Printf(TEXT("LateralRegions_seed_%d"), MultiSeed);
    FVoxelStratePreviewCandidate PreviewCandidate;
    FString PreviewError;
    const FString PreviewRecipe = FString::Printf(
        TEXT("lateral-regions/%d; blend_width=%.3f; archetypes=%d"),
        Manifest.RegionCount, Manifest.BlendWidth,
        DistinctRegionArchetypes(Manifest));
    const bool bPreviewWritten = VF_WriteStratePreviewCandidate(
        PreviewDirectory, MultiCandidate.Index, PreviewRecipe, PreviewGrid, PreviewMetrics,
        PreviewSettings.HeadroomCells, 0.0,
        FString::Printf(TEXT("multi-region law %d/%d; cross-box violations %d"),
                        LawPasses, LawTrials, BoxReport.Violations),
        false, TEXT(""), PreviewCandidate, PreviewError);
    TestTrue(TEXT("multi-region preview renders vertical and plan images"),
             bPreviewWritten && PreviewCandidate.bRendered && PreviewCandidate.bContourRendered);
    TArray<FVoxelStratePreviewCandidate> PreviewCandidates;
    PreviewCandidates.Add(PreviewCandidate);
    FVoxelStratePreviewWindow PreviewWindow = VF_GetStratePreviewWindow(PreviewGrid);
    FString PreviewIndexPath;
    const bool bIndexWritten = VF_WriteStratePreviewIndex(
        PreviewDirectory,
        FString::Printf(TEXT("VoxelForge lateral regions — seed %d"), MultiSeed),
        PreviewWindow, PreviewCandidates, PreviewIndexPath, PreviewError);
    TestTrue(TEXT("multi-region preview contact sheet is written"),
             bIndexWritten && IFileManager::Get().FileExists(*PreviewIndexPath));
    AddInfo(FString::Printf(TEXT("preview path: %s"), *PreviewIndexPath));
    if (!PreviewError.IsEmpty()) { AddInfo(PreviewError); }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
