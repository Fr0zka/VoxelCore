// VoxelForgeTestFixture.h
// Fixture partagée par les tests d'automatisation VoxelForge (Phase 0.5 de OPSTACK-PLAN.md).
// Shared fixture for the VoxelForge automation tests (OPSTACK-PLAN.md, Phase 0.5).
//
// WHY THIS EXISTS
// ---------------
// The interesting invariants (density purity across worker threads, ClassifyTile soundness)
// only fire on the REAL path — UVoxelGenerator::GetDensityAt — because that is where the
// thread_local per-chunk caches live (CP_*, the prepared stacks, the diff slots, the SDF cache).
// Evaluating a stack directly bypasses every one of them and would test almost nothing.
// GetDensityAt in turn needs a live UVoxelStrateManager, whose only
// entry point is Initialize(UVoxelSettings*, int32) reading TSoftObjectPtr pools.
//
// So the fixture builds a whole synthetic world in memory: transient strate definitions →
// a transient UVoxelSettings pointing at them → a real UVoxelStrateManager::Initialize.
//
// ⚠️ KNOWN RISK, stated rather than hidden: the settings hold TSoftObjectPtr, and we point
// them at TRANSIENT objects (/Engine/Transient.<name>). LoadSynchronous() resolves those via
// FindObject, which works for in-memory objects — but it is the one part of this fixture that
// has never been compiled or run. IsValid() below checks the layout actually materialised, and
// every test hard-FAILS with a clear message when it didn't. A silent skip would be worse than
// a failure: it would look like a pass.
//
// Everything is held by TStrongObjectPtr so the GC cannot eat the world mid-test.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"

#include "VoxelTypes.h"
#include "VoxelSettings.h"
#include "VoxelStrateTypes.h"
#include "VoxelStrateDefinition.h"
#include "VoxelStrateManager.h"
#include "VoxelDiffLayer.h"
#include "VoxelGenerator.h"

namespace VoxelForgeTest
{
    /**
     * FTestWorld — a complete, headless VoxelForge world: settings + strate layout +
     * generator + diff layer. No AActor, no UWorld, no PIE.
     *
     * The default layout stacks one strate of EVERY archetype (in ECaveGeneratorType order),
     * so a single fixture exercises all eight archetype stacks and their per-chunk caches,
     * plus the gap-bedrock path when InterStrateGapChunks > 0.
     */
    struct FTestWorld
    {
        TStrongObjectPtr<UVoxelSettings>      Settings;
        TStrongObjectPtr<UVoxelStrateManager> StrateManager;
        TStrongObjectPtr<UVoxelDiffLayer>     DiffLayer;
        TStrongObjectPtr<UVoxelGenerator>     Generator;
        TArray<TStrongObjectPtr<UVoxelStrateDefinition>> Definitions;

        /** World Z (voxel coords) span actually covered by the layout — handy for picking samples. */
        int32 TopChunkZ = 0;
        int32 BottomChunkZ = 0;

        /**
         * Build the world.
         *
         * The default seed stays SMALL, but the reason has changed. It USED to be a workaround:
         * AUDIT §C1 (unbounded `SeedF`) meant a large seed collapsed the noise fields to constants,
         * which would have made a purity test pass trivially for the wrong reason.
         *
         * **§C1 is fixed** (`VoxelHash::SeedOffset` — bounded and site-salted). The small default
         * now just keeps failure messages comparable across tests. A large seed is no longer
         * dangerous — and `VoxelForge.Determinism.LargeSeedSurvives` deliberately passes big ones
         * (up to 2e9) to prove it stays that way.
         */
        void Build(int32 InSeed = 1337, int32 InGapChunks = 2, int32 InStrateHeightInChunks = 4)
        {
            Settings = TStrongObjectPtr<UVoxelSettings>(
                NewObject<UVoxelSettings>(GetTransientPackage(), NAME_None, RF_Transient));
            Settings->Seed = InSeed;
            Settings->InterStrateGapChunks = InGapChunks;
            // WorldRadiusVoxels/EdgeSealThickness intentionally inherit the UVoxelSettings defaults;
            // WorldEdgeSeal overrides them explicitly when it needs a small test world or radius 0.

            // Une strate par archétype. PINNED via FixedStrates, pas via le pool : Initialize()
            // mélange le pool avec le seed, ce qui rendrait la correspondance archétype → Z
            // dépendante du seed et un message d'échec impossible à relire.
            // One strate per archetype, PINNED through FixedStrates rather than the pool:
            // Initialize() shuffles the pool by seed, which would make the archetype → Z mapping
            // seed-dependent and a failure message unreadable. Slot i == Archetypes[i].
            static const ECaveGeneratorType Archetypes[] = {
                ECaveGeneratorType::TunnelNetwork,
                ECaveGeneratorType::FlatPlain,
                ECaveGeneratorType::CrystalChamber,
                ECaveGeneratorType::Maze,
                ECaveGeneratorType::SurfaceWorld,
                ECaveGeneratorType::VerticalShafts,
                ECaveGeneratorType::FloatingIslands,
                ECaveGeneratorType::Underwater,
            };

            const int32 NumArchetypes = (int32)UE_ARRAY_COUNT(Archetypes);
            for (int32 i = 0; i < NumArchetypes; ++i)
            {
                UVoxelStrateDefinition* Def = NewObject<UVoxelStrateDefinition>(
                    GetTransientPackage(), NAME_None, RF_Transient);
                Def->GeneratorType = Archetypes[i];
                // Keep the shared density fixture at the historical 4-chunk test volume unless
                // a caller explicitly asks for the production/default 8-chunk envelope. The
                // refinement and roll tests use the smaller synthetic volume as a bounded memory
                // control; the owner-facing showcase opts into 8 chunks below.
                Def->StrateHeightInChunks = FMath::Max(1, InStrateHeightInChunks);
                // Hard transitions: param blending across a boundary would make "which archetype
                // owns this chunk" ambiguous, and these tests want an unambiguous mapping.
                Def->TransitionType = EVoxelStrateTransition::Hard;
                Definitions.Add(TStrongObjectPtr<UVoxelStrateDefinition>(Def));

                const TSoftObjectPtr<UVoxelStrateDefinition> SoftDef(Def);
                Settings->FixedStrates.Add(i, SoftDef);
                Settings->StratePool.Add(SoftDef);   // fallback if a fixed entry fails to resolve
            }
            Settings->TotalStrates = NumArchetypes;

            StrateManager = TStrongObjectPtr<UVoxelStrateManager>(
                NewObject<UVoxelStrateManager>(GetTransientPackage(), NAME_None, RF_Transient));

            //=================================================================
            // ⚠️ CHAQUE MONDE DE TEST OBTIENT UNE `LayoutVersion` UNIQUE DANS LE PROCESSUS
            //=================================================================
            // Ce n'est pas de la cosmétique, c'est une CONTAMINATION CROISÉE réelle entre tests, et
            // elle n'était jusqu'ici masquée que par un accident.
            //
            // `PassagesVersion` est PAR INSTANCE et part de 0, donc deux `FTestWorld` successifs
            // rendaient tous les deux **1**. Historiquement, les caches `CP_*` de `GetDensityAt`
            // n'avaient que `(ChunkCoord, LayoutVersion)` et le second monde pouvait hériter les
            // params — ET `CP_UseOpStack` — du premier. `DensityCacheOwnerId` ferme maintenant CE
            // chemin prouvé. Les bumps restent ici comme isolation conservatrice des autres caches
            // TLS que cette correction n'a volontairement pas audités ni modifiés.
            //
            // Un compteur de processus donne à chaque monde une version distincte, donc tout cache
            // survivant d'un test à l'autre est forcément invalidé. `Initialize` est déterministe
            // (le pool est mélangé par le seed, les fixed strates sont épinglées), donc le rappeler
            // ne change pas le layout — seulement le compteur.
            //
            // Each test world still gets a process-unique LayoutVersion. DensityCacheOwnerId now
            // prevents the proved CP_* cross-world reuse directly; the version bumps remain as
            // conservative isolation for other TLS caches not audited or changed by that fix.
            static int32 GWorldSerial = 0;
            const int32 Bumps = ++GWorldSerial;
            for (int32 b = 0; b < Bumps; ++b)
            {
                StrateManager->Initialize(Settings.Get(), Settings->Seed);
            }

            DiffLayer = TStrongObjectPtr<UVoxelDiffLayer>(
                NewObject<UVoxelDiffLayer>(GetTransientPackage(), NAME_None, RF_Transient));

            Generator = TStrongObjectPtr<UVoxelGenerator>(
                NewObject<UVoxelGenerator>(GetTransientPackage(), NAME_None, RF_Transient));
            Generator->InitializeSettings(Settings.Get());
            Generator->SetStrateManager(StrateManager.Get());
            Generator->SetDiffLayer(DiffLayer.Get());

            CacheZBounds();
        }

        /** Re-run Initialize (bumps LayoutVersion) — the live-edit path AUDIT C2 is about. */
        void Reinitialize()
        {
            StrateManager->Initialize(Settings.Get(), Settings->Seed);
            CacheZBounds();
        }

        /** False when the soft-pointer resolve failed and no strate layout exists. */
        bool IsValid() const
        {
            return StrateManager.IsValid() && StrateManager->GetNumStrates() > 0;
        }

        FString WhyInvalid() const
        {
            return TEXT("FTestWorld could not build a strate layout. Most likely the ")
                   TEXT("TSoftObjectPtr -> transient UVoxelStrateDefinition resolve failed inside ")
                   TEXT("UVoxelStrateManager::Initialize (LoadSynchronous on /Engine/Transient.*). ")
                   TEXT("See the header comment in VoxelForgeTestFixture.h. This is a FIXTURE ")
                   TEXT("failure, not a generator failure — do not read it as a density bug.");
        }

        /** Voxel-Z of the middle of the layout — a point guaranteed inside a real strate. */
        float MidVoxelZ() const
        {
            return (float)((TopChunkZ + BottomChunkZ) / 2 * CHUNK_SIZE + CHUNK_SIZE / 2);
        }

        /** Layout slot index of each archetype — the Archetypes[] order in Build(), pinned via
         *  FixedStrates so it is stable across seeds. SurfaceWorld matters most: it is the only
         *  archetype ClassifyTile can currently prove anything about (besides bedrock gaps). */
        static constexpr int32 SlotTunnelNetwork  = 0;
        static constexpr int32 SlotFlatPlain      = 1;
        static constexpr int32 SlotCrystalChamber = 2;
        static constexpr int32 SlotMaze           = 3;
        static constexpr int32 SlotSurfaceWorld   = 4;
        static constexpr int32 SlotVerticalShafts = 5;
        static constexpr int32 SlotFloatingIsland = 6;
        static constexpr int32 SlotUnderwater     = 7;

        /** Voxel-Z span of one layout slot. False if the layout is shorter than expected. */
        bool GetSlotVoxelZRange(int32 SlotIndex, int32& OutTopVoxelZ, int32& OutBottomVoxelZ) const
        {
            const TArray<FStrateSlot>& Layout = StrateManager->GetLayout();
            if (!Layout.IsValidIndex(SlotIndex)) { return false; }
            OutTopVoxelZ    = Layout[SlotIndex].TopChunkZ    * CHUNK_SIZE + CHUNK_SIZE - 1;
            OutBottomVoxelZ = Layout[SlotIndex].BottomChunkZ * CHUNK_SIZE;
            return true;
        }

    private:
        void CacheZBounds()
        {
            TopChunkZ = 0;
            BottomChunkZ = 0;
            for (const FStrateSlot& Slot : StrateManager->GetLayout())
            {
                TopChunkZ    = FMath::Max(TopChunkZ,    Slot.TopChunkZ);
                BottomChunkZ = FMath::Min(BottomChunkZ, Slot.BottomChunkZ);
            }
        }
    };

    /**
     * A spread of world sample points that deliberately crosses chunk boundaries, strate
     * boundaries and bedrock gaps — the exact conditions under which a per-chunk cache with a
     * missing key input produces a wrong answer. Integer XY on purpose: that is the lattice the
     * mesher samples, and the one the per-column memos are keyed on.
     */
    inline void BuildSamplePoints(const FTestWorld& World, int32 Count, int32 Seed,
                                  TArray<FVector>& OutPoints)
    {
        OutPoints.Reset(Count);
        FRandomStream Rng(Seed);
        const int32 TopVoxelZ    = World.TopChunkZ    * CHUNK_SIZE + CHUNK_SIZE - 1;
        const int32 BottomVoxelZ = World.BottomChunkZ * CHUNK_SIZE;
        for (int32 i = 0; i < Count; ++i)
        {
            // XY range spans several chunks either side of the origin so the (0,0) spine, the
            // passages and plain interior rock all appear in the sample set.
            const int32 X = Rng.RandRange(-3 * CHUNK_SIZE, 3 * CHUNK_SIZE);
            const int32 Y = Rng.RandRange(-3 * CHUNK_SIZE, 3 * CHUNK_SIZE);
            const int32 Z = Rng.RandRange(BottomVoxelZ, TopVoxelZ);
            OutPoints.Add(FVector((float)X, (float)Y, (float)Z));
        }
    }

    /** Deterministic shuffle of an index array — the "different query order" half of purity. */
    inline void BuildShuffledOrder(int32 Count, int32 Seed, TArray<int32>& OutOrder)
    {
        OutOrder.Reset(Count);
        for (int32 i = 0; i < Count; ++i) { OutOrder.Add(i); }
        FRandomStream Rng(Seed);
        for (int32 i = Count - 1; i > 0; --i)
        {
            OutOrder.Swap(i, Rng.RandRange(0, i));
        }
    }

    /** Bit-exact float compare — NOT FMath::IsNearlyEqual. Window invariance is a bit property
     *  (ARCHITECTURE §8.4): a 1-ULP difference between two chunk windows is a visible seam. */
    inline bool BitEqual(float A, float B)
    {
        return FMath::IsNaN(A) == FMath::IsNaN(B)
            && (FMath::IsNaN(A) || *reinterpret_cast<const uint32*>(&A) == *reinterpret_cast<const uint32*>(&B));
    }
}

#endif // WITH_DEV_AUTOMATION_TESTS
