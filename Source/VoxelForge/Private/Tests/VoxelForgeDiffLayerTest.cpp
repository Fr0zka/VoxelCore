// VoxelForgeDiffLayerTest.cpp
// Phase 0.5 test #3 — LA COUCHE DE DIFF SOUS CONTENTION / DiffLayer under contention.
//
// LE RISQUE / THE RISK:
//   UVoxelDiffLayer::ChunkMods est une TMap LUE par les threads de meshing (via GetDensityAt →
//   GetChunkModsSnapshot) et ÉCRITE par le thread de jeu (ApplyModification / Clear). TMap n'est
//   pas thread-safe : un rehash pendant une lecture est une violation d'accès. Tout est censé
//   passer par ModsLock (FRWLock) — et une AV carve-vs-stream a déjà été corrigée exactement là.
//
//   UVoxelDiffLayer::ChunkMods is a TMap READ by mesher workers (through GetDensityAt →
//   GetChunkModsSnapshot) and WRITTEN by the game thread (ApplyModification / Clear). TMap is not
//   thread-safe: a rehash during a read is an access violation. Everything is meant to go through
//   ModsLock (FRWLock) — and a carve-vs-stream AV was already fixed in exactly this spot.
//
// CE QUE CE TEST PROUVE / WHAT THIS TEST PROVES:
//   1. Aucun crash quand N lecteurs martèlent la couche pendant que le thread de jeu écrit.
//   2. ModsVersion ne RECULE jamais du point de vue d'un lecteur (c'est la clé sur laquelle les
//      caches de snapshot invalident ; une version non monotone rendrait un cache définitivement
//      périmé).
//   3. L'état final est exact : chaque carve appliqué est retrouvable.
//   Le point (1) est le vrai but, et il ne peut être prouvé que statistiquement — un test vert
//   veut dire "pas reproduit ici", pas "impossible". C'est quand même infiniment mieux que rien.
//
//   Point (1) is the real target, and it can only ever be shown statistically — a green run means
//   "not reproduced here", not "impossible". Still infinitely better than nothing.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/Async.h"
#include "HAL/PlatformMisc.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"

#include "VoxelTypes.h"
#include "VoxelDiffLayer.h"

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeDiffLayerContentionTest,
    "VoxelForge.Determinism.DiffLayerContention",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    constexpr int32 NumWrites  = 400;
    constexpr int32 NumChunksX = 8;

    FVoxelModification MakeCarve(int32 Index)
    {
        FVoxelModification Mod;
        Mod.Shape    = EVoxelBrushShape::Sphere;
        Mod.Radius   = 6.0f;
        Mod.Strength = -9.0f;
        Mod.Center   = FVector(
            (float)((Index % NumChunksX) * CHUNK_SIZE + 4),
            (float)(((Index / NumChunksX) % NumChunksX) * CHUNK_SIZE + 4),
            (float)(-((Index / (NumChunksX * NumChunksX)) % 4) * CHUNK_SIZE));
        return Mod;
    }
}

bool FVoxelForgeDiffLayerContentionTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UVoxelDiffLayer> Diff(
        NewObject<UVoxelDiffLayer>(GetTransientPackage(), NAME_None, RF_Transient));
    Diff->SetBudget(/*MaxMods*/ 0, /*MaxRadius*/ 50.0f, /*MaxVolume*/ 0.0f);   // 0 = illimité

    const int32 NumReaders = FMath::Max(3, FMath::Min(8, FPlatformMisc::NumberOfCores() - 1));

    std::atomic<bool>  bStop{ false };
    std::atomic<int32> VersionRegressions{ 0 };
    std::atomic<int64> ReadOps{ 0 };

    // ── Les lecteurs : exactement le mix d'appels que fait le chemin densité d'un worker. ──
    // The readers: exactly the call mix a worker's density path makes.
    TArray<TFuture<void>> Readers;
    Readers.Reserve(NumReaders);
    for (int32 R = 0; R < NumReaders; ++R)
    {
        Readers.Add(Async(EAsyncExecution::Thread, [&, R]()
        {
            uint32 LastVersion = 0;
            int64  LocalOps = 0;
            FRandomStream Rng(9000 + R);
            while (!bStop.load(std::memory_order_relaxed))
            {
                const uint32 V = Diff->GetModsVersion();
                if (V < LastVersion)
                {
                    VersionRegressions.fetch_add(1, std::memory_order_relaxed);
                }
                LastVersion = V;

                const FIntVector Chunk(Rng.RandRange(0, NumChunksX - 1),
                                       Rng.RandRange(0, NumChunksX - 1),
                                       Rng.RandRange(-3, 0));

                // Le fast-reject sans verrou, puis le vrai chemin sous verrou.
                if (Diff->HasAnyMods())
                {
                    Diff->HasAnyModInChunkRange(Chunk - FIntVector(1, 1, 1), Chunk + FIntVector(1, 1, 1));
                    Diff->HasModifications(Chunk);

                    TArray<FVoxelModification> Snapshot;
                    Diff->GetChunkModsSnapshot(Chunk, Snapshot);

                    // Toucher réellement les données copiées : un snapshot qui aliaserait la TMap
                    // (au lieu de la copier) exploserait ici et pas au moment de la copie.
                    // Actually touch the copied data: a snapshot that aliased the TMap instead of
                    // copying it would blow up here rather than at copy time.
                    const float X = (float)(Chunk.X * CHUNK_SIZE + 3);
                    const float Y = (float)(Chunk.Y * CHUNK_SIZE + 3);
                    const float Z = (float)(Chunk.Z * CHUNK_SIZE + 3);
                    const float Sink = UVoxelDiffLayer::EvaluateMods(Snapshot, X, Y, Z)
                                     + Diff->GetDensityOffset(Chunk, X, Y, Z);
                    // Consommer Sink dans une branche que le compilateur ne peut pas prouver morte,
                    // sinon tout le bloc de lecture est éliminé et le test ne teste rien.
                    // Consume Sink in a branch the compiler cannot prove dead, otherwise the whole
                    // read block is optimised away and the test tests nothing.
                    if (Sink == 1.2345678e30f) { ++LocalOps; }
                }
                ++LocalOps;
            }
            ReadOps.fetch_add(LocalOps, std::memory_order_relaxed);
        }));
    }

    // ── Phase 1 : écritures pures. L'état final doit être exact. ──
    for (int32 i = 0; i < NumWrites; ++i)
    {
        const TArray<FIntVector> Touched = Diff->ApplyModification(MakeCarve(i));
        if (Touched.Num() == 0)
        {
            AddError(FString::Printf(
                TEXT("ApplyModification #%d was rejected. The budget should be unlimited here — ")
                TEXT("if this fires, SetBudget(0, ...) no longer means 'no cap'."), i));
            break;
        }
    }

    TestEqual(TEXT("every carve was recorded"), Diff->GetTotalModificationCount(), NumWrites);
    TestTrue(TEXT("the lock-free bHasAnyMods fast-path agrees with the map"), Diff->HasAnyMods());
    TestTrue(TEXT("at least one chunk holds mods"), Diff->GetModifiedChunkCount() > 0);

    // ── Phase 2 : le chemin réellement dangereux — Clear() pendant que les lecteurs tiennent des
    //    itérateurs potentiels. On n'affirme plus de compte ici, seulement la survie + la monotonie.
    //    Phase 2: the genuinely dangerous path — Clear() while readers may hold iterators. No count
    //    assertions here, only survival + monotonicity.
    for (int32 Round = 0; Round < 6; ++Round)
    {
        for (int32 i = 0; i < 60; ++i) { Diff->ApplyModification(MakeCarve(i + Round * 60)); }
        Diff->Clear();
    }

    bStop.store(true, std::memory_order_relaxed);
    for (TFuture<void>& F : Readers) { F.Wait(); }

    AddInfo(FString::Printf(TEXT("%d reader threads completed %lld read rounds against %d writes + 6 clears."),
                            NumReaders, (long long)ReadOps.load(), NumWrites + 360));

    TestEqual(TEXT("ModsVersion never went backwards from a reader's point of view"),
              VersionRegressions.load(), 0);

    if (ReadOps.load() < (int64)NumReaders)
    {
        AddError(TEXT("The reader threads barely ran, so no contention was actually exercised. ")
                 TEXT("The writes finished before the threads started — increase NumWrites or add ")
                 TEXT("a barrier before the writer loop."));
    }

    // Après Clear(), l'état doit être franchement vide (pas « presque »).
    TestFalse(TEXT("Clear() left no mods behind"), Diff->HasAnyMods());
    TestEqual(TEXT("Clear() reset the modified-chunk count"), Diff->GetModifiedChunkCount(), 0);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
