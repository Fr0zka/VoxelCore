// VoxelStrateManager.cpp
// Runtime strate layout generation and queries.

#include "VoxelStrateManager.h"
#include "CoreGlobals.h"  // GIsAutomationTesting — the opt-in diagnostic stays quiet under tests
#include "VoxelSettings.h"
#include "VoxelSeasonAsset.h"
#include "VoxelTypes.h"  // For CHUNK_SIZE, VOXEL_SIZE, WorldToChunkCoord
#include "VoxelCaveMorphology.h"  // For VoxelSDF and VoxelHash
#include "VoxelDensityPrimitives.h"  // Shared passage carve polarity/strength
#include "VoxelDensityProfile.h"  // Opt-in targeted per-voxel attribution
#include "VoxelTerrainOpDefinition.h"  // For UVoxelTerrainOpDefinition::ApplyTo
#include "VoxelBiomeDefinition.h"  // For UVoxelBiomeDefinition (biome context flatten)
#include "UObject/UObjectGlobals.h"

#include <atomic>

namespace
{
    std::atomic<uint64> GNextStrateManagerLifetimeId { 0 };
}

UVoxelStrateManager::UVoxelStrateManager()
    : CacheLifetimeId(GNextStrateManagerLifetimeId.fetch_add(1, std::memory_order_relaxed) + 1)
{
}

#if WITH_EDITOR
#include "VoxelStrateComposer.h"

struct FVoxelStrateComposerSlotOverride
{
    int32 CandidateSeed = 0;
    ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;
    FVoxelStrateArchetypeParams Params;
    bool bUseRecipe = false;
    FVoxelOpStackRecipe Recipe;
    bool bUseRegions = false;
    FVoxelStrateRegionManifest Regions;
};

static void VF_SetComposerRuntimeBounds(
    FVoxelStrateArchetypeParams& Params, float TopWorldZ, float BottomWorldZ)
{
    Params.TunnelNetworkParams.StrateTopWorldZ = TopWorldZ;
    Params.TunnelNetworkParams.StrateBottomWorldZ = BottomWorldZ;
    Params.SlabParams.StrateTopWorldZ = TopWorldZ;
    Params.SlabParams.StrateBottomWorldZ = BottomWorldZ;
    Params.MazeParams.StrateTopWorldZ = TopWorldZ;
    Params.MazeParams.StrateBottomWorldZ = BottomWorldZ;
    Params.SurfaceParams.StrateTopWorldZ = TopWorldZ;
    Params.SurfaceParams.StrateBottomWorldZ = BottomWorldZ;
    Params.VerticalShaftParams.StrateTopWorldZ = TopWorldZ;
    Params.VerticalShaftParams.StrateBottomWorldZ = BottomWorldZ;
    Params.FloatingIslandParams.StrateTopWorldZ = TopWorldZ;
    Params.FloatingIslandParams.StrateBottomWorldZ = BottomWorldZ;
}
#endif

// Fractal Brownian Motion (layered Perlin) along a 1D parameter, ~[-1,1].
// Independent octaves at increasing frequency / decreasing amplitude give an organic,
// non-repeating wander — the key to a worm that SQUIRMS instead of zig-zagging (1D) or
// orbiting (single-octave 2-channel = a spiral). Each axis samples this with its own seed.
static float PassageFBM(float X, float Seed)
{
    float Total = 0.0f, Amp = 1.0f, Freq = 1.0f, MaxV = 0.0f;
    for (int32 O = 0; O < 4; ++O)
    {
        Total += FMath::PerlinNoise3D(FVector(X * Freq + Seed, Seed * 1.7f + O * 13.0f, O * 5.0f)) * Amp;
        MaxV += Amp;
        Amp  *= 0.5f;
        Freq *= 2.0f;
    }
    return (MaxV > 0.0f) ? (Total / MaxV) : 0.0f;
}

namespace
{
    /**
     * One shared passage cache for the tube SDF and landing-floor fill.
     *
     * The source query and all landing dimensions are resolved by GeneratePassages.  This cache
     * only narrows the immutable passage array once per (manager, version, chunk); neither the
     * player-fit stencil nor a topology search can leak into the voxel loop.
     */
    struct FPassageEvaluationCache
    {
        const UVoxelStrateManager* Owner = nullptr;
        uint64 OwnerLifetimeId = 0;
        FIntVector Chunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
        uint32 Version = 0xFFFFFFFFu;
        TArray<int32> Nearby;
    };

    FPassageEvaluationCache& VF_GetPassageEvaluationCache()
    {
        thread_local FPassageEvaluationCache Cache;
        return Cache;
    }

    const TArray<int32>& VF_GetNearbyPassages(
        const UVoxelStrateManager* Manager,
        const FIntVector& ChunkCoord)
    {
        FPassageEvaluationCache& Cache = VF_GetPassageEvaluationCache();
        const uint32 Version = Manager ? Manager->GetLayoutVersion() : 0u;
        const uint64 LifetimeId = Manager ? Manager->GetCacheLifetimeId() : 0;
        if (Cache.Owner == Manager && Cache.OwnerLifetimeId == LifetimeId
            && Cache.Chunk == ChunkCoord && Cache.Version == Version)
        {
            return Cache.Nearby;
        }

        Cache.Owner = Manager;
        Cache.OwnerLifetimeId = LifetimeId;
        Cache.Chunk = ChunkCoord;
        Cache.Version = Version;
        Cache.Nearby.Reset();
        if (!Manager) return Cache.Nearby;

        const FVector ChunkCenter(
            (ChunkCoord.X + 0.5f) * (float)CHUNK_SIZE,
            (ChunkCoord.Y + 0.5f) * (float)CHUNK_SIZE,
            (ChunkCoord.Z + 0.5f) * (float)CHUNK_SIZE);
        const float ChunkRadius = (float)CHUNK_SIZE * 0.8660254f + 3.0f;
        const TArray<FVoxelPassage>& Passages = Manager->GetPassages();
        for (int32 PassageIndex = 0; PassageIndex < Passages.Num(); ++PassageIndex)
        {
            const FVoxelPassage& Passage = Passages[PassageIndex];
            const float Reach = Passage.BoundRadius + ChunkRadius;
            if (FVector::DistSquared(ChunkCenter, Passage.BoundCenter) <= Reach * Reach)
            {
                Cache.Nearby.Add(PassageIndex);
            }
        }
        return Cache.Nearby;
    }
}

static float VF_BoundarySealThicknessForDefinition(
    const UVoxelStrateDefinition& Definition)
{
    switch (Definition.GeneratorType)
    {
    case ECaveGeneratorType::TunnelNetwork:
    case ECaveGeneratorType::Underwater:
        return Definition.GenerationParams.BoundarySealThickness;
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
    default:
        return 0.0f;
    }
}

/**
 * The default inter-strate tunnel is a carved tube plus an explicit support slab.  The tube's
 * rounded SDF guarantees clearance, but it cannot guarantee a standable surface over a source
 * field or a disturbance that was solid before the passage post.  Project the query into the
 * nearest horizontal control segment and return the conservative floor carried by that segment.
 * This is a fixed-size arithmetic loop over the already-built descriptor; it performs no source
 * search, allocation, or topology work.
 */
static bool VF_IsWalkableTunnelFloor(
    const FVoxelPassage& Passage,
    const FVector& Position)
{
    if (!Passage.bWalkableTunnelContract
        || Passage.ControlPoints.Num() < 2
        || Passage.ControlRadii.Num() != Passage.ControlPoints.Num())
    {
        return false;
    }

    float FloorZ = 0.0f;
    float SupportRadius = 0.0f;
    if (!VoxelPassageGeometry::ProjectWalkableTunnelFloor(
            Passage.ControlPoints, Passage.ControlRadii, Position,
            FloorZ, SupportRadius))
    {
        return false;
    }

    return Position.Z <= FloorZ + KINDA_SMALL_NUMBER
        && Position.Z > FloorZ - VoxelPassageGeometry::LandingFloorThicknessVoxels;
}

static bool VF_IsWalkableTunnelAir(
    const FVoxelPassage& Passage,
    const FVector& Position)
{
    if (!Passage.bWalkableTunnelContract
        || Passage.ControlPoints.Num() < 2
        || Passage.ControlRadii.Num() != Passage.ControlPoints.Num())
    {
        return false;
    }

    float FloorZ = 0.0f;
    float SupportRadius = 0.0f;
    if (!VoxelPassageGeometry::ProjectWalkableTunnelFloor(
            Passage.ControlPoints, Passage.ControlRadii, Position,
            FloorZ, SupportRadius))
    {
        return false;
    }

    // The carved tube is wider than this guaranteed core.  The core is deliberately expressed as
    // a vertical prism over the projected floor: it gives the final-density player stencil a
    // stable air volume even when a source archetype or a disturbance would otherwise refill the
    // shallow part of the rounded SDF.  The floor writer below owns the closed lower face.
    const float AirHeight = 2.0f * SupportRadius;
    return Position.Z > FloorZ
        + VoxelPassageGeometry::WalkableTunnelFloorAirClearanceVoxels
        && Position.Z < FloorZ + AirHeight - KINDA_SMALL_NUMBER;
}

static bool VF_FindWalkableTunnelAir(
    const UVoxelStrateManager* Manager,
    const FVector& Position,
    int32& OutPassageIndex,
    float& OutFloorZ,
    float& OutSupportRadius)
{
    OutPassageIndex = INDEX_NONE;
    OutFloorZ = 0.0f;
    OutSupportRadius = 0.0f;
    if (Manager == nullptr)
    {
        return false;
    }

    const FIntVector ChunkCoord(
        FMath::FloorToInt(Position.X / (float)CHUNK_SIZE),
        FMath::FloorToInt(Position.Y / (float)CHUNK_SIZE),
        FMath::FloorToInt(Position.Z / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(Manager, ChunkCoord);
    const TArray<FVoxelPassage>& Passages = Manager->GetPassages();
    for (const int32 PassageIndex : Nearby)
    {
        if (!Passages.IsValidIndex(PassageIndex))
        {
            continue;
        }
        const FVoxelPassage& Passage = Passages[PassageIndex];
        if (FVector::DistSquared(Position, Passage.BoundCenter) > Passage.BoundRadiusSq
            || !VF_IsWalkableTunnelAir(Passage, Position))
        {
            continue;
        }
        VoxelPassageGeometry::ProjectWalkableTunnelFloor(
            Passage.ControlPoints, Passage.ControlRadii, Position,
            OutFloorZ, OutSupportRadius);
        OutPassageIndex = PassageIndex;
        return true;
    }
    return false;
}

static bool VF_IsAnyPassageFloorAt(
    const UVoxelStrateManager* Manager,
    const FVector& Position)
{
    if (Manager == nullptr)
    {
        return false;
    }
    const FIntVector ChunkCoord(
        FMath::FloorToInt(Position.X / (float)CHUNK_SIZE),
        FMath::FloorToInt(Position.Y / (float)CHUNK_SIZE),
        FMath::FloorToInt(Position.Z / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(Manager, ChunkCoord);
    const TArray<FVoxelPassage>& Passages = Manager->GetPassages();
    for (const int32 PassageIndex : Nearby)
    {
        if (!Passages.IsValidIndex(PassageIndex))
        {
            continue;
        }
        const FVoxelPassage& Passage = Passages[PassageIndex];
        if (!VoxelPassageGeometry::VerticalShaftConnectorAirMarker()
            && (VF_IsPassageLandingFloor(Position, Passage.UpperLanding)
            || VF_IsPassageLandingFloor(Position, Passage.LowerLanding)
            || VF_IsWalkableTunnelFloor(Passage, Position)))
        {
            return true;
        }
    }
    return false;
}

bool UVoxelStrateManager::Initialize(UVoxelSettings* Settings, int32 WorldSeed)
{
    if (!Settings)
    {
        UE_LOG(LogTemp, Error, TEXT("[StrateManager] No settings provided!"));
        return false;
    }

    FVoxelSeasonManifest SeasonManifest;
    bool bUseSeason = false;
    if (!Settings->Season.IsNull())
    {
        UVoxelSeasonAsset* SeasonAsset = Settings->Season.LoadSynchronous();
        FString SeasonReport;
        if (SeasonAsset == nullptr
            || !SeasonAsset->LoadManifest(SeasonManifest, SeasonReport))
        {
            UE_LOG(LogTemp, Error,
                TEXT("[StrateManager] Season asset is assigned but unusable: %s"),
                SeasonAsset ? *SeasonReport : *Settings->Season.ToString());
            return false; // Fail closed: falling back to the authored pool would diverge peers.
        }
        bUseSeason = true;
        WorldSeed = SeasonManifest.Seed;
    }

    StrateLayout.Empty();
    SeasonStrates.Empty();
    ActiveSeasonContentHash.Reset();
#if WITH_EDITOR
    // A full layout rebuild discards any temporary walk-through candidate. The world calls this
    // under FScopedGenerationPause, so no worker can observe the map while it is being cleared.
    ComposerOverrides.Reset();
#endif

    const int32 TotalStrates = bUseSeason
        ? SeasonManifest.Strates.Num() : Settings->TotalStrates;
    const int32 LayoutGapChunks = bUseSeason
        ? SeasonManifest.InterStrateGapChunks
        : FMath::Max(0, Settings->InterStrateGapChunks);

    //=========================================================================
    // STEP 1: Build shuffled pool (seed-based randomization)
    //=========================================================================
    // Copy the pool and shuffle it deterministically using the world seed.
    // Fixed strates are excluded from the shuffle — they always use their
    // assigned definition regardless of seed.

    using FLoadedPoolEntry = TPair<FString, UVoxelStrateDefinition*>;
    TArray<FLoadedPoolEntry> ShuffledPool;
    if (!bUseSeason)
    {
        for (const TSoftObjectPtr<UVoxelStrateDefinition>& SoftPtr : Settings->StratePool)
        {
            // Load the asset (synchronous for now — could be async later)
            UVoxelStrateDefinition* Def = SoftPtr.LoadSynchronous();
            if (Def)
            {
                ShuffledPool.Emplace(SoftPtr.ToString(), Def);
            }
        }
    }

    // Sort by the soft asset path before shuffling. Pointer addresses depend on load order and
    // would make the layout machine-dependent. The pool is a set for layout purposes: editor
    // reordering must not change the input sequence seen by Fisher-Yates.
    // Trier par chemin de soft asset avant le shuffle. Les adresses de pointeurs dépendent de
    // l'ordre de chargement et rendraient le layout dépendant de la machine. Le pool est un set
    // pour le layout : réordonner l'asset dans l'éditeur ne doit pas changer l'entrée de Fisher-Yates.
    ShuffledPool.Sort([](const FLoadedPoolEntry& A, const FLoadedPoolEntry& B)
    {
        return FCString::Strcmp(*A.Key, *B.Key) < 0;
    });

    // Seed-based shuffle using Fisher-Yates
    // FRandomStream gives us deterministic random numbers from a seed
    FRandomStream Rng(WorldSeed);
    for (int32 i = ShuffledPool.Num() - 1; i > 0; i--)
    {
        int32 j = Rng.RandRange(0, i);
        ShuffledPool.Swap(i, j);
    }

    //=========================================================================
    // STEP 2: Assign definitions to each strate slot
    //=========================================================================
    // Walk through strate indices 0..TotalStrates-1.
    // Fixed strates use their pinned definition.
    // Random strates cycle through the shuffled pool.

    int32 PoolCursor = 0;  // Current position in the shuffled pool

    // Pre-load fixed strate definitions
    TMap<int32, UVoxelStrateDefinition*> LoadedFixed;
    if (!bUseSeason)
    {
        for (auto& Pair : Settings->FixedStrates)
        {
            UVoxelStrateDefinition* Def = Pair.Value.LoadSynchronous();
            if (Def)
            {
                LoadedFixed.Add(Pair.Key, Def);
            }
        }
    }

    // Current Z position (in chunks). Starts at 0 and goes downward (negative).
    int32 CurrentTopZ = 0;

    for (int32 i = 0; i < TotalStrates; i++)
    {
        FStrateSlot Slot;
        Slot.StrateIndex = i;

        if (bUseSeason)
        {
            const FVoxelSeasonStrate& SeasonStrate = SeasonManifest.Strates[i];
            if (!SeasonStrate.SourceDefinitionPath.IsEmpty())
            {
                TSoftObjectPtr<UVoxelStrateDefinition> SourceDefinition(
                    FSoftObjectPath(SeasonStrate.SourceDefinitionPath));
                UVoxelStrateDefinition* LoadedDefinition = SourceDefinition.LoadSynchronous();
                if (LoadedDefinition == nullptr)
                {
                    UE_LOG(LogTemp, Error,
                        TEXT("[StrateManager] Season slot %d could not load authored definition '%s'."),
                        i, *SeasonStrate.SourceDefinitionPath);
                    StrateLayout.Empty();
                    SeasonStrates.Empty();
                    return false;
                }
                // Keep the authored content/passage/visual bag without mutating the cooked asset,
                // then make the manifest's density identity authoritative below.
                Slot.Definition = DuplicateObject<UVoxelStrateDefinition>(LoadedDefinition, this);
            }
            else
            {
                // The current density manifest has no wider content record. Keep all consumers
                // supplied with a stable definition object, but do not guess an authored theme.
                Slot.Definition = NewObject<UVoxelStrateDefinition>(this, NAME_None, RF_Transient);
                Slot.Definition->StrateName = FText::FromString(
                    FString::Printf(TEXT("Season %d Strate %d"), SeasonManifest.Season, i));
                Slot.Definition->TransitionType = EVoxelStrateTransition::Hard;
                // Passage configuration is not part of schema v2. Inventing the UObject default
                // here would add an unreviewed tunnel to every generated boundary and make the
                // runtime field differ from the field that passed composition. The origin spine
                // remains the guaranteed connection until a later schema explicitly stores these.
                Slot.Definition->PassageConfig.Connections = 0;
            }

            Slot.Definition->GeneratorType = SeasonStrate.Archetype;
            Slot.Definition->bUseOperatorStack = SeasonStrate.bUsesRecipe;
            Slot.Definition->StrateHeightInChunks = SeasonStrate.HeightInChunks;
            Slot.Definition->GenerationParams = SeasonStrate.Params.TunnelNetworkParams;
            Slot.Definition->SlabParams = SeasonStrate.Params.SlabParams;
            Slot.Definition->MazeParams = SeasonStrate.Params.MazeParams;
            Slot.Definition->SurfaceParams = SeasonStrate.Params.SurfaceParams;
            Slot.Definition->VerticalShaftParams = SeasonStrate.Params.VerticalShaftParams;
            Slot.Definition->FloatingIslandParams = SeasonStrate.Params.FloatingIslandParams;

            Slot.HeightInChunks = SeasonStrate.HeightInChunks;
            Slot.TopChunkZ = SeasonStrate.TopWorldZ / CHUNK_SIZE - 1;
            Slot.BottomChunkZ = SeasonStrate.BottomWorldZ / CHUNK_SIZE;
            if (SeasonStrate.TopWorldZ % CHUNK_SIZE != 0
                || SeasonStrate.BottomWorldZ % CHUNK_SIZE != 0
                || Slot.TopChunkZ != CurrentTopZ
                || Slot.BottomChunkZ != CurrentTopZ - (Slot.HeightInChunks - 1))
            {
                UE_LOG(LogTemp, Error,
                    TEXT("[StrateManager] Season slot %d bounds do not describe the declared stacked layout."), i);
                StrateLayout.Empty();
                SeasonStrates.Empty();
                return false;
            }
            SeasonStrates.Add(SeasonStrate);
        }
        else
        {
            // Pick definition: fixed or from pool
            UVoxelStrateDefinition** FixedDef = LoadedFixed.Find(i);
            if (FixedDef && *FixedDef)
            {
                Slot.Definition = *FixedDef;
            }
            else if (ShuffledPool.Num() > 0)
            {
                // Cycle through the pool (wraps around if more strates than pool entries)
                Slot.Definition = ShuffledPool[PoolCursor % ShuffledPool.Num()].Value;
                PoolCursor++;
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("[StrateManager] No strate definitions available for slot %d!"), i);
                continue;
            }

            // Compute Z range from definition's height
            Slot.HeightInChunks = Slot.Definition->StrateHeightInChunks;
            Slot.TopChunkZ = CurrentTopZ;
            Slot.BottomChunkZ = CurrentTopZ - (Slot.HeightInChunks - 1);
        }

        // Move the cursor down for the next strate, leaving a solid-bedrock gap of
        // InterStrateGapChunks chunks between this strate and the next.
        CurrentTopZ = Slot.BottomChunkZ - 1 - LayoutGapChunks;

        StrateLayout.Add(Slot);

        UE_LOG(LogTemp, Log, TEXT("[StrateManager] Strate %d: '%s' | Z chunks [%d to %d] | %d chunks tall"),
            i,
            *Slot.Definition->StrateName.ToString(),
            Slot.TopChunkZ,
            Slot.BottomChunkZ,
            Slot.HeightInChunks);
    }

    // Diagnostic de configuration, une seule fois par construction de layout. SurfaceWorld est
    // volontairement exclu : son chemin T1.d exact-lattice ne dépend pas de ce drapeau.
    // Configuration diagnostic once per layout build. SurfaceWorld is deliberately excluded:
    // its exact-lattice T1.d path does not depend on this flag.
    int32 NumCaveSlots = 0;
    int32 NumOperatorStackDisabledCaves = 0;
    for (const FStrateSlot& Slot : StrateLayout)
    {
        const ECaveGeneratorType SlotArchetype = bUseSeason
            ? SeasonStrates[Slot.StrateIndex].Archetype : Slot.Definition->GeneratorType;
        if (!Slot.Definition || SlotArchetype == ECaveGeneratorType::SurfaceWorld)
        {
            continue;
        }

        ++NumCaveSlots;
        const bool bUsesStack = bUseSeason
            ? SeasonStrates[Slot.StrateIndex].bUsesRecipe : Slot.Definition->bUseOperatorStack;
        if (!bUsesStack)
        {
            ++NumOperatorStackDisabledCaves;
        }
    }

    // ⚠️ WARNING EN ÉDITEUR/JEU, JAMAIS EN TEST. Les tests `Determinism.*` construisent
    // DÉLIBÉRÉMENT un monde non opt-in — c'est leur oracle de comparaison — et le framework
    // d'automatisation compte un Warning comme un échec. Un diagnostic ne doit pas casser la suite
    // qu'il est censé éclairer. Le message reste écrit UNE fois : seule la verbosité change.
    // Warning in editor/game where it is actionable, never in tests: the Determinism.* tests build
    // a non-opted-in world ON PURPOSE as their comparison oracle, and the automation framework
    // treats a Warning as a failure. One message, two verbosities.
    const bool bQuietDiagnostic = GIsAutomationTesting;

    if (NumOperatorStackDisabledCaves > 0)
    {
        const FString Summary = FString::Printf(
            TEXT("[StrateManager] Operator-stack opt-in: %d/%d cave layout slots have Use Operator Stack disabled. These slots cannot use operator-stack ClassifyBox/T1.d; enable the asset setting on the listed definitions if that is intended."),
            NumOperatorStackDisabledCaves, NumCaveSlots);

        if (bQuietDiagnostic) { UE_LOG(LogTemp, Verbose, TEXT("%s"), *Summary); }
        else                  { UE_LOG(LogTemp, Warning, TEXT("%s"), *Summary); }
    }
    else
    {
        UE_LOG(LogTemp, Log,
            TEXT("[StrateManager] Operator-stack opt-in: all %d cave layout slots have Use Operator Stack enabled."),
            NumCaveSlots);
    }

    for (const FStrateSlot& Slot : StrateLayout)
    {
        const bool bSeasonStack = bUseSeason
            && SeasonStrates[Slot.StrateIndex].bUsesRecipe;
        const ECaveGeneratorType SlotArchetype = bUseSeason
            ? SeasonStrates[Slot.StrateIndex].Archetype : Slot.Definition->GeneratorType;
        if (!Slot.Definition
            || SlotArchetype == ECaveGeneratorType::SurfaceWorld
            || (bUseSeason ? bSeasonStack : Slot.Definition->bUseOperatorStack))
        {
            continue;
        }

        const FString Line = FString::Printf(
            TEXT("[StrateManager]   cave slot=%d name='%s' Z chunks=[%d to %d] bUseOperatorStack=false"),
            Slot.StrateIndex,
            *Slot.Definition->StrateName.ToString(),
            Slot.TopChunkZ,
            Slot.BottomChunkZ);

        if (bQuietDiagnostic) { UE_LOG(LogTemp, Verbose, TEXT("%s"), *Line); }
        else                  { UE_LOG(LogTemp, Warning, TEXT("%s"), *Line); }
    }

    CachedSeed = WorldSeed;
    bOpenSurfaceEntry = Settings->bOpenSurfaceEntry;
    OriginSpineRadius = bUseSeason
        ? SeasonManifest.OriginSpineRadius : Settings->OriginSpineRadius;
    InterStrateGapChunks = LayoutGapChunks;
    if (bUseSeason)
    {
        ActiveSeasonContentHash = SeasonManifest.ContentHash;
    }
    // Passage shape/count is per-strate now (UVoxelStrateDefinition::PassageConfig).

    //=========================================================================
    // STEP 3: Load terrain operation assets
    //=========================================================================
    // Each strate definition references terrain ops as soft pointers.
    // We load them synchronously here so they're available during generation.
    // Without this, BuildParamsFromDefinition's Entry.Operation.Get() would
    // return null if the assets haven't been loaded yet.
    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (!Slot.Definition) continue;
        if (bUseSeason && SeasonStrates[Slot.StrateIndex].bUsesRecipe) continue;

        for (const FStrateTerrainOpEntry& Entry : Slot.Definition->TerrainOperations)
        {
            if (!Entry.Operation.IsNull())
            {
                Entry.Operation.LoadSynchronous();
            }
        }
    }

    UE_LOG(LogTemp, Log, TEXT("[StrateManager] Initialized %d strates (seed=%d)"),
        StrateLayout.Num(), WorldSeed);

    // Generate passages between consecutive strates
    GeneratePassages();
    return bUseSeason ? IsUsingSeason() : true;
}

#if WITH_EDITOR
const FVoxelStrateComposerSlotOverride* UVoxelStrateManager::FindComposerOverride(
    int32 StrateIndex) const
{
    const TSharedPtr<FVoxelStrateComposerSlotOverride>* Found = ComposerOverrides.Find(StrateIndex);
    return Found != nullptr ? Found->Get() : nullptr;
}

bool UVoxelStrateManager::SetComposerOverrideForStrate(
    int32 StrateIndex, int32 CandidateSeed, ECaveGeneratorType Archetype,
    const FVoxelStrateArchetypeParams& Params, bool bUseRecipe,
    const FVoxelOpStackRecipe* Recipe, FString& OutError,
    const FVoxelStrateRegionManifest* InRegions)
{
    OutError.Reset();

    const FStrateSlot* TargetSlot = nullptr;
    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (Slot.StrateIndex == StrateIndex)
        {
            TargetSlot = &Slot;
            break;
        }
    }
    if (TargetSlot == nullptr || TargetSlot->Definition == nullptr)
    {
        OutError = FString::Printf(TEXT("Strate index %d is not present in the live layout."), StrateIndex);
        return false;
    }

    switch (Archetype)
    {
    case ECaveGeneratorType::TunnelNetwork:
    case ECaveGeneratorType::FlatPlain:
    case ECaveGeneratorType::CrystalChamber:
    case ECaveGeneratorType::Maze:
    case ECaveGeneratorType::SurfaceWorld:
    case ECaveGeneratorType::VerticalShafts:
    case ECaveGeneratorType::FloatingIslands:
    case ECaveGeneratorType::Underwater:
        break;
    default:
        OutError = TEXT("The composer returned an unsupported strate archetype.");
        return false;
    }

    if (bUseRecipe && Recipe == nullptr)
    {
        OutError = TEXT("A structure candidate did not provide a recipe.");
        return false;
    }

    TSharedPtr<FVoxelStrateComposerSlotOverride> Override =
        MakeShared<FVoxelStrateComposerSlotOverride>();
    Override->CandidateSeed = CandidateSeed;
    Override->Archetype = Archetype;
    Override->Params = Params;
    VF_SetComposerRuntimeBounds(
        Override->Params,
        (float)(TargetSlot->TopChunkZ + 1) * CHUNK_SIZE,
        (float)TargetSlot->BottomChunkZ * CHUNK_SIZE);
    Override->bUseRecipe = bUseRecipe;
    if (Recipe != nullptr)
    {
        Override->Recipe = *Recipe;
    }

    if (InRegions != nullptr && InRegions->RegionCount > 1)
    {
        if (!InRegions->IsValid())
        {
            OutError = InRegions->FailureReason.IsEmpty()
                ? TEXT("The composer returned an invalid lateral region manifest.")
                : InRegions->FailureReason;
            return false;
        }
        Override->bUseRegions = true;
        Override->Regions = *InRegions;
        VF_RekeyStrateRegionManifest(Override->Regions, CandidateSeed, StrateIndex);
        Override->Regions.StrateTopWorldZ = (float)(TargetSlot->TopChunkZ + 1) * CHUNK_SIZE;
        Override->Regions.StrateBottomWorldZ = (float)TargetSlot->BottomChunkZ * CHUNK_SIZE;
        VF_SetStrateArchetypeRuntimeBounds(Override->Params,
                                           Override->Regions.StrateTopWorldZ,
                                           Override->Regions.StrateBottomWorldZ);
        for (FVoxelStrateRegion& Region : Override->Regions.Regions)
        {
            VF_SetStrateArchetypeRuntimeBounds(Region.ArchetypeParams,
                                               Override->Regions.StrateTopWorldZ,
                                               Override->Regions.StrateBottomWorldZ);
        }
        Override->Regions.bHasGlobalStructuralParams = true;
    }

    // Only one slot is overridden at a time. Keeping this map small also makes the worker-side
    // copy-on-chunk-refetch cheap. Passage geometry is deliberately not regenerated: the layout
    // and its passages are unchanged, and this is the same manager state used by the offline
    // candidate measurement harness.
    ComposerOverrides.Reset();
    ComposerOverrides.Add(StrateIndex, MoveTemp(Override));

    // This is both the passage-shortlist invalidation counter and the cache key used by the
    // generator's per-chunk params/stack memos. The world has already paused all readers.
    ++PassagesVersion;
    return true;
}

bool UVoxelStrateManager::GetComposerOverrideForChunk(
    const FIntVector& ChunkCoord, int32& OutCandidateSeed,
    ECaveGeneratorType& OutArchetype, FVoxelStrateArchetypeParams& OutParams,
    bool& bOutUseRecipe, FVoxelOpStackRecipe& OutRecipe) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0)
    {
        return false;
    }

    const FVoxelStrateComposerSlotOverride* Override =
        FindComposerOverride(StrateLayout[SlotIdx].StrateIndex);
    if (Override == nullptr)
    {
        return false;
    }

    OutCandidateSeed = Override->CandidateSeed;
    OutArchetype = Override->Archetype;
    OutParams = Override->Params;
    bOutUseRecipe = Override->bUseRecipe;
    OutRecipe = Override->Recipe;
    return true;
}

bool UVoxelStrateManager::GetComposerRegionOverrideForChunk(
    const FIntVector& ChunkCoord, FVoxelStrateRegionManifest& OutRegions) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0)
    {
        return false;
    }

    const FVoxelStrateComposerSlotOverride* Override =
        FindComposerOverride(StrateLayout[SlotIdx].StrateIndex);
    if (Override == nullptr || !Override->bUseRegions)
    {
        return false;
    }
    OutRegions = Override->Regions;
    return true;
}

#endif

bool UVoxelStrateManager::GetRecipeForChunk(
    const FIntVector& ChunkCoord, int32& OutRecipeSeed,
    ECaveGeneratorType& OutArchetype, FVoxelStrateArchetypeParams& OutParams,
    FVoxelOpStackRecipe& OutRecipe) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0) return false;

#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override =
            FindComposerOverride(StrateLayout[SlotIdx].StrateIndex))
    {
        if (!Override->bUseRecipe || Override->bUseRegions) return false;
        OutRecipeSeed = Override->CandidateSeed;
        OutArchetype = Override->Archetype;
        OutParams = Override->Params;
        OutRecipe = Override->Recipe;
        return true;
    }
#endif
    if (SeasonStrates.IsValidIndex(SlotIdx))
    {
        const FVoxelSeasonStrate& Strate = SeasonStrates[SlotIdx];
        if (!Strate.bUsesRecipe || Strate.bUsesRegions) return false;
        OutRecipeSeed = Strate.Seed;
        OutArchetype = Strate.Archetype;
        OutParams = Strate.Params;
        OutRecipe = Strate.Recipe;
        return true;
    }
    return false;
}

//=============================================================================
// PASSAGE GENERATION
//=============================================================================

void UVoxelStrateManager::GeneratePassages()
{
    Passages.Empty();

    if (StrateLayout.Num() < 1) return;

    // Deterministic per-value hashes from the world seed. Every draw is keyed by its boundary
    // strate index, connection index, and a unique salt, so one passage cannot shift another.
    // Hachages déterministes par valeur depuis le seed du monde. Chaque tirage est indexé par la
    // strate de frontière, la connexion et un sel unique : un passage ne peut plus décaler l'autre.
    const uint32 PassageSeed = static_cast<uint32>(CachedSeed) ^ 0x50A55A6Eu;  // "PASSAGE"
    constexpr uint32 PassageSaltAngle      = 0xA1100001u;
    constexpr uint32 PassageSaltDistance   = 0xA1100002u;
    constexpr uint32 PassageSaltUpperReach = 0xA1100003u;
    constexpr uint32 PassageSaltLowerReach = 0xA1100004u;
    constexpr uint32 PassageSaltWormFreq   = 0xA1100005u;
    constexpr uint32 PassageSaltNoiseX     = 0xA1100006u;
    constexpr uint32 PassageSaltNoiseY     = 0xA1100007u;
    constexpr uint32 PassageSaltNoiseZ     = 0xA1100008u;
    constexpr uint32 PassageSaltPhase      = 0xA1100009u;
    constexpr uint32 PassageSaltBendFreq   = 0xA110000Au;
    constexpr uint32 PassageSaltUpperDoor  = 0xA110000Bu;
    constexpr uint32 PassageSaltLowerDoor  = 0xA110000Cu;

    int32 TotalPassages = 0;
    int32 NumAimedAtUpperPlayerFit = 0;
    int32 NumAimedAtLowerPlayerFit = 0;
    TSet<FString> NoQueryArchetypes;

    const auto ArchetypeName = [](ECaveGeneratorType Archetype)
    {
        if (const UEnum* ArchetypeEnum = StaticEnum<ECaveGeneratorType>())
        {
            return ArchetypeEnum->GetNameStringByValue(static_cast<int64>(Archetype));
        }
        return FString::Printf(TEXT("Value_%d"), static_cast<int32>(Archetype));
    };

    const auto MaxLateralSnapFor = [](const UVoxelStrateDefinition& Definition) -> float
    {
        switch (Definition.GeneratorType)
        {
        case ECaveGeneratorType::Maze:
            // One maze lattice cell is the largest deliberate correction. It keeps a shortcut
            // within the same local maze neighborhood and protects its configured spine-distance
            // distribution from silently becoming a long-range teleport.
            return FMath::Max(Definition.MazeParams.CellSize, 1.0f);

        case ECaveGeneratorType::VerticalShafts:
            // A density of 0.6 does not guarantee an occupied cell in the immediate grid
            // neighbourhood. Allow the nearest occupied shaft to be two grid cells away while
            // remaining a bounded local query; the landing query still returns a pose inside
            // that shaft's deterministic feature core, so this cannot create a non-topological
            // mouth.
            return FMath::Max(2.0f * Definition.VerticalShaftParams.ShaftSpacing, 1.0f);

        case ECaveGeneratorType::FloatingIslands:
            // One island grid spacing protects the intentional radial placement while still
            // allowing a passage to reach a neighboring blob rather than an arbitrary far island.
            return FMath::Max(Definition.FloatingIslandParams.IslandSpacing, 1.0f);

        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            // Une salle est une CIBLE ÉPARSE en XY, exactement comme un couloir de labyrinthe : le
            // budget doit donc être réel. Zéro ici était un bug — la requête trouvait la salle la
            // plus proche puis atterrissait à côté d'elle dans 92,8 % des cas (balayage d'un million
            // de seeds). Un espacement de salles est le voisinage local naturel.
            //
            // A room is an XY-SPARSE target, exactly like a maze corridor, so the budget must be
            // real. Zero here was the bug: the query found the nearest room and then landed beside
            // it 92.8% of the time (million-seed sweep). One room spacing is the natural local
            // neighbourhood, and it protects the configured spine-distance distribution.
            return FMath::Max(Definition.GenerationParams.RoomSpacing, 1.0f);

        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
        default:
            // Les slabs sont une bande de vide CONTINUE en XY : le XY demandé est déjà à
            // l'intérieur, donc aucun déplacement latéral n'est nécessaire. Zéro est correct ici.
            // A slab's void band is XY-CONTINUOUS, so the requested XY is already inside it and no
            // lateral movement is needed. Zero is correct here, unlike for rooms.
            return 0.0f;
        }
    };

    const auto BoundarySealThicknessFor = [](const UVoxelStrateDefinition& Definition) -> float
    {
        switch (Definition.GeneratorType)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return Definition.GenerationParams.BoundarySealThickness;

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

        default:
            return 0.0f;
        }
    };

    //=========================================================================
    // INTER-STRATE PASSAGES: tunnels connecting consecutive strates.
    // Each passage is randomly assigned one of 5 types, which determines
    // its shape, radius, and control point layout.
    //=========================================================================
    for (int32 i = 0; i < StrateLayout.Num() - 1; i++)
    {
        const FStrateSlot& Upper = StrateLayout[i];
        const FStrateSlot& Lower = StrateLayout[i + 1];

        // This (upper) strate's PassageConfig controls the progression tunnel to the layer below.
        // Each strate already owns a finite (0,0) landing room; this passage is the connection
        // that opens the next room rather than a second vertical spine.
        const UVoxelStrateDefinition* UpperDef = Upper.Definition;
        if (!UpperDef) continue;
        const FStratePassageConfig& Cfg = UpperDef->PassageConfig;

        // Upper strate floor and lower strate ceiling (differ when there's a bedrock gap).
        const float UpperTopZ    = (float)(Upper.TopChunkZ + 1) * CHUNK_SIZE;
        const float UpperBottomZ = (float)(Upper.BottomChunkZ) * CHUNK_SIZE;
        const float LowerTopZ    = (float)(Lower.TopChunkZ + 1) * CHUNK_SIZE;
        const float UpperMax = (float)Upper.HeightInChunks * CHUNK_SIZE * 0.9f;
        const float LowerMax = (float)Lower.HeightInChunks * CHUNK_SIZE * 0.9f;

        const float DistLo = FMath::Min(Cfg.DistanceMin, Cfg.DistanceMax);
        const float DistHi = FMath::Max(Cfg.DistanceMin, Cfg.DistanceMax);
        // The origin room owns the future straight shaft. Keep the progression mouth outside its
        // carve/blend envelope, then choose a deterministic point in the authored annulus. If an
        // asset has an invalid inner radius, repair only that radius; the outer authored radius is
        // preserved unless it is too small to contain the required inner edge.
        const float OriginLandingHalfWidth =
            VoxelPassageGeometry::LandingHalfWidthForRadius(OriginSpineRadius);
        const float MinimumDescentDistance = FMath::Max(
            OriginLandingHalfWidth + VoxelPassageGeometry::LandingCarveBlendVoxels,
            VoxelPassageGeometry::MinimumTurnFloorWidthVoxels);
        const float PlacementLo = FMath::Max(DistLo, MinimumDescentDistance);
        const float PlacementHi = FMath::Max(DistHi, PlacementLo);

        const int32 Conns = FMath::Max(0, Cfg.Connections);
        for (int32 c = 0; c < Conns; c++)
        {
            const auto PassageRandom01 = [PassageSeed, i, c](uint32 Salt)
            {
                return VoxelHash::ToFloat01(VoxelHash::Cell(i, c, PassageSeed ^ Salt));
            };
            const auto PassageRandomRange = [&PassageRandom01](float Min, float Max, uint32 Salt)
            {
                return FMath::Lerp(Min, Max, PassageRandom01(Salt));
            };

            FVoxelPassage Passage;
            Passage.UpperStrateIndex = i;
            Passage.LowerStrateIndex = i + 1;
            // The authored style is also reflected in the legacy passage type.  In particular,
            // only Straight receives the walkable-tunnel contract; a future style roll cannot
            // silently inherit the default's player-fit promise.
            switch (Cfg.Style)
            {
            case EVoxelPassageStyle::Straight:
                Passage.PassageType = EVoxelPassageType::SlopedTunnel;
                break;
            case EVoxelPassageStyle::Spiral:
                Passage.PassageType = EVoxelPassageType::SpiralDescent;
                break;
            case EVoxelPassageStyle::Cascading:
                Passage.PassageType = EVoxelPassageType::CascadingDrops;
                break;
            case EVoxelPassageStyle::Worm:
            default:
                Passage.PassageType = EVoxelPassageType::CrackCrevice;
                break;
            }

            // PLACEMENT: random angle, distance from the (0,0) spine within a deterministic
            // annulus. It is deliberately not the spine: that room is reserved for the future
            // surface shaft, while this passage opens a separate room in the strate network.
            const float Angle = PassageRandom01(PassageSaltAngle) * (2.0f * PI);
            const float Distance = PassageRandomRange(PlacementLo, PlacementHi, PassageSaltDistance);
            const float PX = FMath::Cos(Angle) * Distance;
            const float PY = FMath::Sin(Angle) * Distance;

            ++TotalPassages;

            // Ask each mouth's own strate for a source-level landing point. These queries are pure
            // and safe during Initialize: they do not construct an operator stack or call back
            // into the manager. Cave room graphs use the queried strate's seed from
            // MakeStrateSeed; slab, maze, shaft, and island fields use the world seed consumed by
            // their source. The two queries are deliberately independent: a passage never reads
            // another passage's endpoint or any live layout state.
            FVector SuggestedUpperPoint = FVector::ZeroVector;
            bool bAimedUpperAtPlayerFitPoint = false;
            if (UpperDef)
            {
                const bool bUsesRoomSeed =
                    UpperDef->GeneratorType == ECaveGeneratorType::TunnelNetwork
                    || UpperDef->GeneratorType == ECaveGeneratorType::Underwater;
                const int32 UpperQuerySeed = bUsesRoomSeed
                    ? static_cast<int32>(VoxelCaveMorphology::MakeStrateSeed(
                        static_cast<uint32>(CachedSeed), Upper.StrateIndex))
                    : CachedSeed;

                bAimedUpperAtPlayerFitPoint = VF_SuggestLandingPoint(
                    UpperDef->GeneratorType,
                    UpperDef->GenerationParams,
                    UpperDef->SlabParams,
                    UpperDef->MazeParams,
                    UpperDef->VerticalShaftParams,
                    UpperDef->FloatingIslandParams,
                    UpperQuerySeed,
                    UpperTopZ,
                    UpperBottomZ,
                    PX,
                    PY,
                    MaxLateralSnapFor(*UpperDef),
                    SuggestedUpperPoint,
                    CachedSeed);

                if (bAimedUpperAtPlayerFitPoint)
                {
                    ++NumAimedAtUpperPlayerFit;
                }
                else
                {
                    NoQueryArchetypes.Add(ArchetypeName(UpperDef->GeneratorType));
                }
            }

            FVector SuggestedLowerPoint = FVector::ZeroVector;
            bool bAimedLowerAtPlayerFitPoint = false;
            const UVoxelStrateDefinition* LowerDef = Lower.Definition;
            if (LowerDef)
            {
                const bool bUsesRoomSeed =
                    LowerDef->GeneratorType == ECaveGeneratorType::TunnelNetwork
                    || LowerDef->GeneratorType == ECaveGeneratorType::Underwater;
                const int32 LowerQuerySeed = bUsesRoomSeed
                    ? static_cast<int32>(VoxelCaveMorphology::MakeStrateSeed(
                        static_cast<uint32>(CachedSeed), Lower.StrateIndex))
                    : CachedSeed;

                const float MaxLateralSnap = MaxLateralSnapFor(*LowerDef);
                bAimedLowerAtPlayerFitPoint = VF_SuggestLandingPoint(
                    LowerDef->GeneratorType,
                    LowerDef->GenerationParams,
                    LowerDef->SlabParams,
                    LowerDef->MazeParams,
                    LowerDef->VerticalShaftParams,
                    LowerDef->FloatingIslandParams,
                    LowerQuerySeed,
                    LowerTopZ,
                    (float)(Lower.BottomChunkZ) * CHUNK_SIZE,
                    PX,
                    PY,
                    MaxLateralSnap,
                    SuggestedLowerPoint,
                    CachedSeed);

                if (bAimedLowerAtPlayerFitPoint)
                {
                    ++NumAimedAtLowerPlayerFit;
                }
                else
                {
                    NoQueryArchetypes.Add(ArchetypeName(LowerDef->GeneratorType));
                }
            }

            // LENGTH: reach into each strate, capped to the interior.
            const float UpperReach = FMath::Min(
                PassageRandomRange(Cfg.ReachMin, Cfg.ReachMax, PassageSaltUpperReach), UpperMax);
            const float LowerReach = FMath::Min(
                PassageRandomRange(Cfg.ReachMin, Cfg.ReachMax, PassageSaltLowerReach), LowerMax);
            float TopZ = UpperBottomZ + UpperReach;
            float BottomZ = LowerTopZ - LowerReach;
            float UpperX = PX;
            float UpperY = PY;
            float LowerX = PX;
            float LowerY = PY;
            Passage.RequestedUpperPoint = FVector(PX, PY, TopZ);
            Passage.RequestedLowerPoint = FVector(PX, PY, BottomZ);

            if (bAimedUpperAtPlayerFitPoint)
            {
                const float UpperSealThickness = BoundarySealThicknessFor(*UpperDef);
                const float InnerBottomZ = UpperBottomZ + UpperSealThickness;
                const float InnerTopZ = UpperTopZ - UpperSealThickness;
                if (FMath::IsFinite(SuggestedUpperPoint.X)
                    && FMath::IsFinite(SuggestedUpperPoint.Y)
                    && FMath::IsFinite(SuggestedUpperPoint.Z)
                    && FMath::IsFinite(InnerBottomZ)
                    && FMath::IsFinite(InnerTopZ)
                    && InnerBottomZ < InnerTopZ)
                {
                    // VF_SuggestLandingPoint already guarantees a strict interior answer. Keep a
                    // tiny margin in the final clamp so a future source query cannot land on a
                    // seal boundary through rounding.
                    UpperX = SuggestedUpperPoint.X;
                    UpperY = SuggestedUpperPoint.Y;
                    const float StrictMargin = FMath::Min(
                        KINDA_SMALL_NUMBER, (InnerTopZ - InnerBottomZ) * 0.25f);
                    TopZ = FMath::Clamp(
                        SuggestedUpperPoint.Z,
                        InnerBottomZ + StrictMargin,
                        InnerTopZ - StrictMargin);
                }
            }

            if (bAimedLowerAtPlayerFitPoint)
            {
                const float LowerSealThickness = BoundarySealThicknessFor(*LowerDef);
                const float LowerBottomZ = (float)(Lower.BottomChunkZ) * CHUNK_SIZE;
                const float InnerBottomZ = LowerBottomZ + LowerSealThickness;
                const float InnerTopZ = LowerTopZ - LowerSealThickness;
                if (FMath::IsFinite(SuggestedLowerPoint.X)
                    && FMath::IsFinite(SuggestedLowerPoint.Y)
                    && FMath::IsFinite(SuggestedLowerPoint.Z)
                    && FMath::IsFinite(InnerBottomZ)
                    && FMath::IsFinite(InnerTopZ)
                    && InnerBottomZ < InnerTopZ)
                {
                    // VF_SuggestLandingPoint already guarantees a strict interior answer. Keep a
                    // tiny margin in the final clamp so a future source query cannot land on a
                    // seal boundary through rounding.
                    LowerX = SuggestedLowerPoint.X;
                    LowerY = SuggestedLowerPoint.Y;
                    const float StrictMargin = FMath::Min(
                        KINDA_SMALL_NUMBER, (InnerTopZ - InnerBottomZ) * 0.25f);
                    BottomZ = FMath::Clamp(
                        SuggestedLowerPoint.Z,
                        InnerBottomZ + StrictMargin,
                        InnerTopZ - StrictMargin);
                }
            }

            const int32 Segments = FMath::Clamp(Cfg.Segments, 1, 48);
            Passage.ControlPoints.Reset();
            Passage.ControlRadii.Reset();
            Passage.ControlPoints.Reserve(Segments + 1);
            Passage.ControlRadii.Reserve(Segments + 1);

            // WIDTH profile: mouth radius at the ends, mid radius in the centre (taper/bulge).
            auto RadiusAt = [&](float t) { return FMath::Lerp(Cfg.MouthRadius, Cfg.MidRadius, FMath::Sin(t * PI)); };

            // Per-passage shape seeds.
            const float WormFreq = PassageRandomRange(0.8f, 1.8f, PassageSaltWormFreq);   // (vertical wobble only)
            const float NSeedX = PassageRandomRange(0.0f, 500.0f, PassageSaltNoiseX);
            const float NSeedY = PassageRandomRange(0.0f, 500.0f, PassageSaltNoiseY);
            const float NSeedZ = PassageRandomRange(0.0f, 500.0f, PassageSaltNoiseZ);
            const float PhaseA = PassageRandom01(PassageSaltPhase) * (2.0f * PI);
            // Base fBM frequency for the worm's wander (octaves add finer detail on top).
            const float BendFreq = PassageRandomRange(1.5f, 2.5f, PassageSaltBendFreq);

            for (int32 s = 0; s <= Segments; s++)
            {
                const float T = (float)s / (float)Segments;
                float Z = FMath::Lerp(TopZ, BottomZ, T);
                const float Env = FMath::Sin(T * PI);  // 0 at both ends → mouths stay anchored
                float OX = 0.0f, OY = 0.0f;

                switch (Cfg.Style)
                {
                case EVoxelPassageStyle::Straight:
                    // No authored lateral offset. A snapped lower mouth can still make this
                    // segment slanted, so the endpoint interpolation below remains intentional.
                    break;

                case EVoxelPassageStyle::Spiral:
                {
                    const float Ang = PhaseA + T * Cfg.SpiralTurns * 2.0f * PI;
                    OX = FMath::Cos(Ang) * Cfg.SpiralRadius * Env;
                    OY = FMath::Sin(Ang) * Cfg.SpiralRadius * Env;
                    break;
                }

                case EVoxelPassageStyle::Cascading:
                {
                    // Switchback staircase: each tread offsets in a new deterministic direction.
                    const int32 Steps = FMath::Clamp(Cfg.CascadeSteps, 1, 16);
                    const int32 Idx = FMath::Min((int32)(T * Steps), Steps - 1);
                    const float SA = PhaseA + (float)Idx * 2.39996f;  // golden-angle spread
                    OX = FMath::Cos(SA) * Cfg.CascadeLedge * Env;
                    OY = FMath::Sin(SA) * Cfg.CascadeLedge * Env;
                    break;
                }

                case EVoxelPassageStyle::Worm:
                default:
                {
                    // SQUIRM: displace the descent independently on X and Y with multi-octave
                    // fBM (different seeds → uncorrelated). Independent fBM per axis is a true
                    // 2D organic wander — it curls and meanders "here and there" rather than
                    // oscillating along one line (zig-zag) or orbiting the axis (spiral).
                    // Flat-top envelope keeps full motion along the length but anchors the mouths.
                    const float WormEnv = FMath::Clamp(FMath::Sin(T * PI) * 3.0f, 0.0f, 1.0f);
                    OX = PassageFBM(T * BendFreq, NSeedX)         * VOXEL_NOISE_SCALE * Cfg.Wander * WormEnv;
                    OY = PassageFBM(T * BendFreq, NSeedY + 53.0f) * VOXEL_NOISE_SCALE * Cfg.Wander * WormEnv;
                    break;
                }
                }

                // Vertical wobble (all styles): dips/rises along the descent, anchored at ends.
                if (Cfg.VerticalWobble > 0.0f)
                {
                    const float NZ = FMath::PerlinNoise3D(FVector(T * WormFreq * 1.3f + NSeedZ, NSeedZ * 0.5f, 27.0f));
                    Z += NZ * VOXEL_NOISE_SCALE * Cfg.VerticalWobble * Env;
                }

                const float BaseX = FMath::Lerp(UpperX, LowerX, T);
                const float BaseY = FMath::Lerp(UpperY, LowerY, T);
                FVector ControlPoint(BaseX + OX, BaseY + OY, Z);
                // The style envelope is mathematically zero at a mouth, but evaluating sin(PI)
                // leaves a tiny float residue. Pin both endpoints so the full query result is the
                // actual aimed mouth, including any lateral snap, and never merely an approximation.
                if (s == 0)
                {
                    ControlPoint = FVector(UpperX, UpperY, TopZ);
                }
                else if (s == Segments)
                {
                    ControlPoint = FVector(LowerX, LowerY, BottomZ);
                }
                Passage.ControlPoints.Add(ControlPoint);
                Passage.ControlRadii.Add(RadiusAt(T));
            }

            // The old control-point endpoints were also the player's standing points. That made
            // a sloped/vertical tube open directly under the capsule. Keep the queried points as
            // explicit standing anchors, and move only the tube's first/last point to a doorway
            // tangent to the landing floor. The endpoint-to-anchor distinction is the geometry
            // that turns a mouth into a place.
            auto DoorDirectionFrom = [](const FVector& Delta, float Angle) -> FVector
            {
                FVector Direction(Delta.X, Delta.Y, 0.0f);
                if (!Direction.Normalize())
                {
                    Direction = FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
                    if (!Direction.Normalize())
                    {
                        Direction = FVector(1.0f, 0.0f, 0.0f);
                    }
                }
                return Direction;
            };

            const FVector UpperDoorDirection = DoorDirectionFrom(
                Passage.ControlPoints.Num() > 1
                    ? Passage.ControlPoints[1] - Passage.ControlPoints[0]
                    : FVector::ZeroVector,
                PassageRandom01(PassageSaltUpperDoor) * 2.0f * PI);
            const FVector LowerDoorDirection = DoorDirectionFrom(
                Passage.ControlPoints.Num() > 1
                    ? Passage.ControlPoints[Passage.ControlPoints.Num() - 2]
                        - Passage.ControlPoints.Last()
                    : FVector::ZeroVector,
                PassageRandom01(PassageSaltLowerDoor) * 2.0f * PI);

            // A source-fit answer is the landing itself. Keep it local to the requested room: the
            // (0,0) landing is reserved for the future straight shaft, so no radial connector is
            // synthesized here.
            Passage.UpperLanding = VF_BuildPassageLanding(
                FVector(UpperX, UpperY, TopZ),
                Cfg.MouthRadius,
                UpperDoorDirection,
                UpperTopZ,
                UpperBottomZ,
                UpperDef ? BoundarySealThicknessFor(*UpperDef) : 0.0f,
                bAimedUpperAtPlayerFitPoint);
            Passage.LowerLanding = VF_BuildPassageLanding(
                FVector(LowerX, LowerY, BottomZ),
                Cfg.MouthRadius,
                LowerDoorDirection,
                LowerTopZ,
                (float)(Lower.BottomChunkZ) * CHUNK_SIZE,
                LowerDef ? BoundarySealThicknessFor(*LowerDef) : 0.0f,
                bAimedLowerAtPlayerFitPoint);
            Passage.UpperPoint = Passage.UpperLanding.StandingPoint;
            Passage.LowerPoint = Passage.LowerLanding.StandingPoint;

            if (Cfg.Style == EVoxelPassageStyle::Straight
                && Passage.UpperLanding.HalfWidth > 0.0f
                && Passage.LowerLanding.HalfWidth > 0.0f)
            {
                // BASE CONNECTION: a walkable two-leg switchback.  A direct line between the
                // mouths would often be nearly vertical because both requests are placed at the
                // same XY radius.  Put the turn on the perpendicular bisector and give each leg
                // at least half of the horizontal run required by the named 15-degree law.
                // This is deliberately a construction rule, not a seed-dependent observation.
                const float SwitchbackAngle =
                    PassageRandom01(PassageSaltUpperDoor) * 2.0f * PI;
                FVector2D SwitchbackDirection(
                    FMath::Cos(SwitchbackAngle), FMath::Sin(SwitchbackAngle));
                if (!SwitchbackDirection.Normalize())
                {
                    SwitchbackDirection = FVector2D(1.0f, 0.0f);
                }

                const FVector DoorDirection(
                    SwitchbackDirection.X, SwitchbackDirection.Y, 0.0f);
                Passage.UpperLanding.DoorDirection = DoorDirection;
                // A real switchback turns around: the lower doorway faces back toward the upper
                // leg. Keeping both doors on the same side would make the two sloped legs retrace
                // one XY line at different heights, leaving the floor projection ambiguous.
                const FVector TunnelLowerDoorDirection = -DoorDirection;
                Passage.LowerLanding.DoorDirection = TunnelLowerDoorDirection;
                const float UpperDoorOffset = FMath::Max(
                    Passage.UpperLanding.HalfWidth - 1.0f, 0.0f);
                const float LowerDoorOffset = FMath::Max(
                    Passage.LowerLanding.HalfWidth - 1.0f, 0.0f);
                const float SafeMouthRadius = FMath::Max(
                    FMath::Abs(Cfg.MouthRadius), 1.0f);
                Passage.UpperLanding.DoorPoint =
                    Passage.UpperLanding.StandingPoint + DoorDirection * UpperDoorOffset;
                Passage.UpperLanding.DoorPoint.Z =
                    Passage.UpperLanding.FloorZ + SafeMouthRadius;
                Passage.LowerLanding.DoorPoint =
                    Passage.LowerLanding.StandingPoint
                        + TunnelLowerDoorDirection * LowerDoorOffset;
                Passage.LowerLanding.DoorPoint.Z =
                    Passage.LowerLanding.FloorZ + SafeMouthRadius;

                const FVector UpperDoor = Passage.UpperLanding.DoorPoint;
                const FVector LowerDoor = Passage.LowerLanding.DoorPoint;
                // Keep a short level apron beyond each room floor. The door anchor is one voxel
                // inside the room's support square; a diagonal door direction can therefore need
                // several voxels before it leaves that square and its four-voxel SDF blend.
                // The level apron must clear the room half-width, the tube radius, and the
                // smooth carve band before the sloped leg begins.  The final six-voxel transition
                // is measured from that clear point back toward the turn, so include it too.  For
                // the default 8-voxel half-width / 6-voxel mouth this is 17 voxels (4.25 m), not
                // the old 6-voxel minimum that left the sloped capsule inside the landing room.
                const float LandingApron = FMath::Max(
                    VoxelPassageGeometry::WalkableTunnelLandingApronVoxels,
                    VoxelPassageGeometry::WalkableTunnelTurnTransitionVoxels
                        + SafeMouthRadius
                        + VoxelPassageGeometry::LandingCarveBlendVoxels
                        + 1.0f);
                const FVector UpperApronEnd = UpperDoor
                    + DoorDirection * LandingApron;
                const FVector LowerApronBegin = LowerDoor
                    + TunnelLowerDoorDirection * LandingApron;
                const float VerticalDrop = FMath::Abs(UpperDoor.Z - LowerDoor.Z);
                const float UpperFloorZ = VoxelPassageGeometry::TunnelFloorZ(
                    UpperDoor, Cfg.MouthRadius);
                const float LowerFloorZ = VoxelPassageGeometry::TunnelFloorZ(
                    LowerDoor, Cfg.MouthRadius);
                const float MidFloorZ = 0.5f * (UpperFloorZ + LowerFloorZ);
                const FVector2D RampStartMid(
                    0.5f * (UpperApronEnd.X + LowerApronBegin.X),
                    0.5f * (UpperApronEnd.Y + LowerApronBegin.Y));
                // The turn offset must be perpendicular to the actual two-apron endpoint chord,
                // not merely perpendicular to the standing-point chord.  Opposite-facing doors
                // add their offsets to that chord; using the old direction could make the two
                // sloped legs nearly collinear and bring the lower leg back beside the upper door.
                FVector2D TurnDirection(
                    LowerApronBegin.Y - UpperApronEnd.Y,
                    -(LowerApronBegin.X - UpperApronEnd.X));
                if (!TurnDirection.Normalize())
                {
                    TurnDirection = FVector2D(-DoorDirection.Y, DoorDirection.X);
                    if (!TurnDirection.Normalize())
                    {
                        TurnDirection = FVector2D(0.0f, 1.0f);
                    }
                }
                // There are two sides on which the switchback can turn. Choose the side whose
                // two sloped legs initially move away from their own landing rooms; the old fixed
                // sign could send a slope back through a room floor before reaching open space.
                FVector2D UpperOutward(
                    UpperApronEnd.X - Passage.UpperLanding.StandingPoint.X,
                    UpperApronEnd.Y - Passage.UpperLanding.StandingPoint.Y);
                FVector2D LowerOutward(
                    LowerApronBegin.X - Passage.LowerLanding.StandingPoint.X,
                    LowerApronBegin.Y - Passage.LowerLanding.StandingPoint.Y);
                UpperOutward.Normalize();
                LowerOutward.Normalize();
                const float TurnOffsetForScore =
                    0.5f * VoxelPassageGeometry::RequiredHorizontalRunForFloorDrop(
                        UpperFloorZ - 0.5f * (UpperFloorZ + LowerFloorZ),
                        0.5f * (UpperFloorZ + LowerFloorZ) - LowerFloorZ)
                    + VoxelPassageGeometry::WalkableTunnelTurnTransitionVoxels;
                const FVector2D TurnMidPlus(
                    RampStartMid.X + TurnDirection.X * TurnOffsetForScore,
                    RampStartMid.Y + TurnDirection.Y * TurnOffsetForScore);
                const FVector2D TurnMidMinus(
                    RampStartMid.X - TurnDirection.X * TurnOffsetForScore,
                    RampStartMid.Y - TurnDirection.Y * TurnOffsetForScore);
                const auto OutwardTurnScore = [
                    &UpperOutward, &LowerOutward, &UpperApronEnd, &LowerApronBegin]
                    (const FVector2D& Candidate) -> float
                {
                    return FVector2D::DotProduct(
                               Candidate - FVector2D(
                                   UpperApronEnd.X, UpperApronEnd.Y), UpperOutward)
                        + FVector2D::DotProduct(
                               FVector2D(LowerApronBegin.X, LowerApronBegin.Y) - Candidate,
                               LowerOutward);
                };
                if (OutwardTurnScore(TurnMidMinus) > OutwardTurnScore(TurnMidPlus))
                {
                    TurnDirection *= -1.0f;
                }
                const float UpperFloorDrop = UpperFloorZ - MidFloorZ;
                const float LowerFloorDrop = MidFloorZ - LowerFloorZ;
                const float RequiredHorizontalRun =
                    VoxelPassageGeometry::RequiredHorizontalRunForFloorDrop(
                        UpperFloorDrop, LowerFloorDrop);
                // Move the level turn farther than half the required run. Each slope then loses
                // only the named transition length to its level segment and still retains the
                // full horizontal run required by the 15-degree floor law.
                const float TurnTransition =
                    VoxelPassageGeometry::WalkableTunnelTurnTransitionVoxels;
                const float TurnOffset =
                    0.5f * RequiredHorizontalRun + TurnTransition;
                const FVector2D FloorSafeDoorMid(
                    RampStartMid.X
                        + TurnDirection.X * TurnOffset,
                    RampStartMid.Y
                        + TurnDirection.Y * TurnOffset);
                FVector2D UpperSlopeDirection(
                    FloorSafeDoorMid.X - UpperApronEnd.X,
                    FloorSafeDoorMid.Y - UpperApronEnd.Y);
                if (!UpperSlopeDirection.Normalize())
                {
                    UpperSlopeDirection = FVector2D(
                        DoorDirection.X, DoorDirection.Y);
                }
                FVector2D LowerSlopeDirection(
                    LowerApronBegin.X - FloorSafeDoorMid.X,
                    LowerApronBegin.Y - FloorSafeDoorMid.Y);
                if (!LowerSlopeDirection.Normalize())
                {
                    LowerSlopeDirection = -UpperSlopeDirection;
                }
                const FVector2D UpperSlopeStartXY(
                    UpperApronEnd.X + UpperSlopeDirection.X * TurnTransition,
                    UpperApronEnd.Y + UpperSlopeDirection.Y * TurnTransition);
                const FVector2D LowerSlopeEndXY(
                    LowerApronBegin.X - LowerSlopeDirection.X * TurnTransition,
                    LowerApronBegin.Y - LowerSlopeDirection.Y * TurnTransition);
                const FVector UpperApronEndPoint(
                    UpperApronEnd.X, UpperApronEnd.Y,
                    UpperFloorZ + FMath::Abs(Cfg.MouthRadius));
                const FVector UpperSlopeStart(
                    UpperSlopeStartXY.X, UpperSlopeStartXY.Y,
                    UpperFloorZ + FMath::Abs(Cfg.MouthRadius));
                const FVector MidPoint(
                    FloorSafeDoorMid.X, FloorSafeDoorMid.Y,
                    MidFloorZ + FMath::Abs(Cfg.MidRadius));
                const FVector LowerSlopeEnd(
                    LowerSlopeEndXY.X, LowerSlopeEndXY.Y,
                    LowerFloorZ + FMath::Abs(Cfg.MouthRadius));
                const FVector LowerApronBeginPoint(
                    LowerApronBegin.X, LowerApronBegin.Y,
                    LowerFloorZ + FMath::Abs(Cfg.MouthRadius));

                Passage.ControlPoints.Reset(7);
                Passage.ControlRadii.Reset(7);
                Passage.ControlPoints.Add(UpperDoor);
                Passage.ControlPoints.Add(UpperApronEndPoint);
                Passage.ControlPoints.Add(UpperSlopeStart);
                Passage.ControlPoints.Add(MidPoint);
                Passage.ControlPoints.Add(LowerSlopeEnd);
                Passage.ControlPoints.Add(LowerApronBeginPoint);
                Passage.ControlPoints.Add(LowerDoor);
                Passage.ControlRadii.Add(Cfg.MouthRadius);
                Passage.ControlRadii.Add(Cfg.MouthRadius);
                Passage.ControlRadii.Add(Cfg.MouthRadius);
                Passage.ControlRadii.Add(Cfg.MidRadius);
                Passage.ControlRadii.Add(Cfg.MouthRadius);
                Passage.ControlRadii.Add(Cfg.MouthRadius);
                Passage.ControlRadii.Add(Cfg.MouthRadius);

                Passage.PassageType = EVoxelPassageType::SlopedTunnel;
                Passage.bWalkableTunnelContract = true;
                Passage.TunnelMaxGradientDegrees =
                    VoxelPassageGeometry::WalkableTunnelMaxGradientDegrees;
                Passage.TunnelVerticalDropVoxels = VerticalDrop;
                Passage.TunnelRequiredHorizontalRunVoxels = RequiredHorizontalRun;
                Passage.TunnelHorizontalPathLengthVoxels = 0.0f;
                for (int32 ControlIndex = 0;
                     ControlIndex + 1 < Passage.ControlPoints.Num();
                     ++ControlIndex)
                {
                    Passage.TunnelHorizontalPathLengthVoxels += FVector2D(
                        Passage.ControlPoints[ControlIndex + 1].X
                            - Passage.ControlPoints[ControlIndex].X,
                        Passage.ControlPoints[ControlIndex + 1].Y
                            - Passage.ControlPoints[ControlIndex].Y).Size();
                }
                Passage.TunnelMinimumClearWidthVoxels = 2.0f * FMath::Max(
                    FMath::Min(FMath::Abs(Cfg.MouthRadius), FMath::Abs(Cfg.MidRadius)),
                    0.0f);
                Passage.TunnelClearHeightVoxels = Passage.TunnelMinimumClearWidthVoxels;
            }
            else if (Passage.ControlPoints.Num() > 0)
            {
                Passage.ControlPoints[0] = Passage.UpperLanding.DoorPoint;
                Passage.ControlPoints.Last() = Passage.LowerLanding.DoorPoint;
            }
            Passage.Radius = 0.0f;
            for (const float ControlRadius : Passage.ControlRadii)
            {
                Passage.Radius = FMath::Max(Passage.Radius, ControlRadius);
            }
            // Fallback / bounds if an invalid authored width produced no positive profile.
            Passage.Radius = FMath::Max(Passage.Radius, FMath::Max(Cfg.MouthRadius, Cfg.MidRadius));

            // Bounding sphere over the tube, both rooms, and both floor slabs (+ widest radius +
            // blend) for culling. Under-sizing this sphere would cull a real landing and leave a
            // sealed pocket, so the room's full box diagonal is included rather than treating the
            // standing anchor as a point.
            {
                FVector BoundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
                FVector BoundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
                auto IncludePoint = [&BoundsMin, &BoundsMax](const FVector& Point, float Pad)
                {
                    BoundsMin.X = FMath::Min(BoundsMin.X, Point.X - Pad);
                    BoundsMin.Y = FMath::Min(BoundsMin.Y, Point.Y - Pad);
                    BoundsMin.Z = FMath::Min(BoundsMin.Z, Point.Z - Pad);
                    BoundsMax.X = FMath::Max(BoundsMax.X, Point.X + Pad);
                    BoundsMax.Y = FMath::Max(BoundsMax.Y, Point.Y + Pad);
                    BoundsMax.Z = FMath::Max(BoundsMax.Z, Point.Z + Pad);
                };
                for (const FVector& CP : Passage.ControlPoints)
                {
                    IncludePoint(CP, Passage.Radius + 4.0f);
                }
                const auto IncludeLanding = [&IncludePoint](const FVoxelPassageLanding& Landing)
                {
                    const float Height = FMath::Max(Landing.CeilingZ - Landing.FloorZ, 0.0f);
                    const FVector RoomCenter(
                        Landing.StandingPoint.X,
                        Landing.StandingPoint.Y,
                        (Landing.FloorZ + Landing.CeilingZ) * 0.5f);
                    const float RoomRadius = FMath::Sqrt(
                        2.0f * FMath::Square(Landing.HalfWidth)
                        + 0.25f * FMath::Square(Height))
                        + Landing.FloorThickness + 4.0f;
                    IncludePoint(RoomCenter, RoomRadius);
                };
                IncludeLanding(Passage.UpperLanding);
                IncludeLanding(Passage.LowerLanding);

                const FVector Center = (BoundsMin + BoundsMax) * 0.5f;
                float MaxDistSq = 0.0f;
                MaxDistSq = FMath::Max(MaxDistSq, (float)FVector::DistSquared(Center, BoundsMin));
                MaxDistSq = FMath::Max(MaxDistSq, (float)FVector::DistSquared(Center, BoundsMax));
                const float R = FMath::Sqrt(MaxDistSq);
                Passage.BoundCenter = Center;
                Passage.BoundRadius = R;
                Passage.BoundRadiusSq = R * R;
            }

            Passages.Add(Passage);
        }
    }

    TArray<FString> SortedNoQueryArchetypes;
    for (const FString& Archetype : NoQueryArchetypes)
    {
        SortedNoQueryArchetypes.Add(Archetype);
    }
    SortedNoQueryArchetypes.Sort();
    FString NoQueryList = TEXT("none");
    if (SortedNoQueryArchetypes.Num() > 0)
    {
        NoQueryList = FString::Join(SortedNoQueryArchetypes, TEXT(", "));
    }

    UE_LOG(LogTemp, Log,
        TEXT("[StrateManager] Passage landings: upper %d/%d and lower %d/%d source-fit; %d/%d mouth queries fell back to random reach (archetypes with no query: %s)."),
        NumAimedAtUpperPlayerFit,
        TotalPassages,
        NumAimedAtLowerPlayerFit,
        TotalPassages,
        (TotalPassages * 2) - NumAimedAtUpperPlayerFit - NumAimedAtLowerPlayerFit,
        TotalPassages * 2,
        *NoQueryList);

    //=========================================================================
    // SURFACE ENTRY — the one optional above-ground opening.
    // It opens the top seal and stops at the ceiling of strate 0's origin landing room. It does
    // not continue down the room or into lower strates: every lower seal remains closed until the
    // corresponding progression passage is opened.
    //=========================================================================
    if (bOpenSurfaceEntry && OriginSpineRadius > 0.0f && StrateLayout.Num() > 0)
    {
        const FStrateSlot& Top = StrateLayout[0];
        const float TopZ = (float)(Top.TopChunkZ + 1) * CHUNK_SIZE;
        const float BottomZ = (float)Top.BottomChunkZ * CHUNK_SIZE;
        const float TopSeal = Top.Definition
            ? BoundarySealThicknessFor(*Top.Definition) : 0.0f;
        const VoxelPassageGeometry::FOriginLandingGeometry OriginLanding =
            VoxelPassageGeometry::BuildOriginLandingGeometry(
                TopZ, BottomZ, TopSeal, OriginSpineRadius);
        if (!OriginLanding.bValid)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[StrateManager] Surface entry skipped: top origin landing has no seal-safe room interval."));
        }
        else
        {
            FVoxelPassage Entry;
            Entry.UpperStrateIndex = 0;
            Entry.LowerStrateIndex = 0;
            // Keep the legacy type: this is the explicitly opted-in above-ground vertical opening,
            // not the default inter-strate descent style.
            Entry.PassageType = EVoxelPassageType::VerticalShaft;
            // Match the reserved future shaft envelope exactly. The entry is only opened through
            // the top seal into the finite landing room; lower strate seals remain untouched.
            Entry.Radius = FMath::Max(OriginSpineRadius, 1.0f);
            Entry.UpperPoint = FVector(0.0f, 0.0f, TopZ + CHUNK_SIZE);
            // The capsule's lower tangent meets the room ceiling; it never bores through the
            // room's floor or creates an origin column in strate 0.
            Entry.LowerPoint = FVector(
                0.0f, 0.0f, OriginLanding.CeilingZ + Entry.Radius);
            Entry.ControlPoints.Reset(2);
            Entry.ControlPoints.Add(Entry.UpperPoint);
            Entry.ControlPoints.Add(Entry.LowerPoint);
            Entry.ControlRadii.Reset(2);
            Entry.ControlRadii.Add(Entry.Radius);
            Entry.ControlRadii.Add(Entry.Radius);
            Entry.TunnelClearHeightVoxels = 2.0f * Entry.Radius;
            Entry.TunnelMinimumClearWidthVoxels = 2.0f * Entry.Radius;
            {
                const FVector C = (Entry.UpperPoint + Entry.LowerPoint) * 0.5f;
                const float R = (float)FVector::Dist(C, Entry.UpperPoint) + Entry.Radius + 4.0f;
                Entry.BoundCenter = C;
                Entry.BoundRadius = R;
                Entry.BoundRadiusSq = R * R;
            }
            Passages.Add(Entry);

            UE_LOG(LogTemp, Log,
                TEXT("[StrateManager] Surface entry opening at (0,0) topZ=%.0f roomFloor=%.1f roomCeiling=%.1f R=%.1f"),
                TopZ, OriginLanding.FloorZ, OriginLanding.CeilingZ, Entry.Radius);
        }
    }

    // Invalidate any thread_local per-chunk passage shortlists (see EvaluateModifierSDF).
    ++PassagesVersion;
}

//=============================================================================
// MODIFIER SDF (inter-strate passages)
//=============================================================================

float UVoxelStrateManager::EvaluateModifierSDF(float WorldX, float WorldY, float WorldZ) const
{
    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));

    // The shortlist is rebuilt once for a (manager, version, chunk) and is shared by the tube,
    // landing, and floor paths. No source-fit stencil or topology search is allowed below it.
    const TArray<int32>& Nearby = VF_GetNearbyPassages(this, ChunkCoord);
    if (VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::PassageCandidates,
            static_cast<uint64>(Nearby.Num()));
    }

    if (Nearby.Num() == 0) return FLT_MAX;   // no passage near this chunk → no carve

    float MinSDF = FLT_MAX;
    const float BlendK = 3.0f;  // Smooth blend for passage junctions

    //=========================================================================
    // PASSAGES — tapered capsule chains between strates (per-strate PassageConfig).
    // Each passage is a control-point chain with per-point radii; the (0,0) surface
    // entry is a simple straight tube. A bounding-sphere reject skips far passages.
    //=========================================================================
    const FVector Pos(WorldX, WorldY, WorldZ);
    for (int32 PIdx : Nearby)
    {
        const FVoxelPassage& P = Passages[PIdx];
        // BOUNDING-SPHERE REJECT: skip passages this voxel can't possibly be inside.
        // EvaluateModifierSDF runs PER VOXEL and used to evaluate every passage's full
        // capsule chain unconditionally — the dominant lag source once passages became
        // 12-segment worms. Now far passages cost a single squared-distance compare.
        if (FVector::DistSquared(Pos, P.BoundCenter) > P.BoundRadiusSq) continue;
        if (VoxelDensityProfile::AreCountersEnabled())
        {
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::PassageEvaluated);
        }

        if (P.ControlPoints.Num() >= 2)
        {
            // Tapered capsule chain along the control points. ControlRadii (if present)
            // gives the per-point width so the tunnel can flare at the mouths and pinch
            // in the middle; otherwise the uniform Radius is used.
            const bool bTaper = (P.ControlRadii.Num() == P.ControlPoints.Num());
            float PassageSDF = FLT_MAX;
            for (int32 j = 0; j < P.ControlPoints.Num() - 1; j++)
            {
                const float rA = bTaper ? P.ControlRadii[j]     : P.Radius;
                const float rB = bTaper ? P.ControlRadii[j + 1] : P.Radius;
                const float SegSDF = VoxelSDF::TaperedCapsule(
                    Pos, P.ControlPoints[j], P.ControlPoints[j + 1], rA, rB);
                PassageSDF = VoxelSDF::SmoothMin(PassageSDF, SegSDF, BlendK);
            }
            MinSDF = VoxelSDF::SmoothMin(MinSDF, PassageSDF, BlendK);
        }
        else
        {
            // Fallback: straight uniform tube Upper→Lower (e.g. the (0,0) surface entry).
            const float PassageSDF = VoxelSDF::Capsule(Pos, P.UpperPoint, P.LowerPoint, P.Radius);
            MinSDF = VoxelSDF::SmoothMin(MinSDF, PassageSDF, BlendK);
        }

        // A landing is a real room with a hard flat-floor half-space, not a sphere around the
        // tube endpoint. It is the same fixed SDF geometry selected during GeneratePassages; it
        // never performs a source query here and does not synthesize a radial root connector.
        const float UpperLandingSDF = VF_EvaluatePassageLandingSDF(Pos, P.UpperLanding);
        const float LowerLandingSDF = VF_EvaluatePassageLandingSDF(Pos, P.LowerLanding);
        MinSDF = VoxelSDF::SmoothMin(MinSDF, UpperLandingSDF, BlendK);
        MinSDF = VoxelSDF::SmoothMin(MinSDF, LowerLandingSDF, BlendK);
    }

    return MinSDF;
}

void UVoxelStrateManager::ApplyPassageModifier(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageModifier);
    const float ModSDF = EvaluateModifierSDF(WorldX, WorldY, WorldZ);
    VF_ApplyPassageCarving(Density, ModSDF, BaseDensity, SealThickness);
    ApplyPassageLandingAir(Density, WorldX, WorldY, WorldZ, BaseDensity, SealThickness);

    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(this, ChunkCoord);
    const FVector Position(WorldX, WorldY, WorldZ);
    for (const int32 PassageIndex : Nearby)
    {
        const FVoxelPassage& Passage = Passages[PassageIndex];
        if (!VoxelPassageGeometry::VerticalShaftConnectorAirMarker()
            && (VF_IsPassageLandingFloor(Position, Passage.UpperLanding)
            || VF_IsPassageLandingFloor(Position, Passage.LowerLanding)
            || VF_IsWalkableTunnelFloor(Passage, Position)))
        {
            // This is the one bidirectional part of PassageCarveOp: a floor is a proved solid
            // support slab. It is deliberately applied after the air carve so a tube can never
            // tunnel through the floor and leave the player over a void.
            Density = FMath::Max(Density, BaseDensity);
            break;
        }
    }

    // A neighbouring passage may own a floor at this XY while this point is in the air core of
    // the current walkable tunnel.  Reassert tunnel air after all floor posts so overlap cannot
    // turn a valid route into a solid plug.  At the tunnel's own floor the air predicate is false,
    // so the support plane remains solid.
    ApplyPassageTunnelAir(Density, WorldX, WorldY, WorldZ, BaseDensity, SealThickness);
}

void UVoxelStrateManager::ApplyPassageCarvingOnly(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageModifier);
    const float ModSDF = EvaluateModifierSDF(WorldX, WorldY, WorldZ);
    VF_ApplyPassageCarving(Density, ModSDF, BaseDensity, SealThickness);
}

void UVoxelStrateManager::ApplyPassageLandingAir(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageLandingAir);
    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(this, ChunkCoord);
    if (Nearby.Num() == 0) return;

    const FVector Position(WorldX, WorldY, WorldZ);
    // A smooth room blend may overlap the end of a ramp.  The analytic support plane owns that
    // overlap; otherwise landing air can erase the tunnel's final-density floor after the tunnel
    // air post has deliberately stopped above it.
    if (VF_IsAnyPassageFloorAt(this, Position))
    {
        return;
    }
    float MinLandingSDF = FLT_MAX;
    for (const int32 PassageIndex : Nearby)
    {
        const FVoxelPassage& Passage = Passages[PassageIndex];
        if (FVector::DistSquared(Position, Passage.BoundCenter) > Passage.BoundRadiusSq)
        {
            continue;
        }
        const FVoxelPassageLanding* Landings[] = {
            &Passage.UpperLanding, &Passage.LowerLanding };
        for (const FVoxelPassageLanding* Landing : Landings)
        {
            const float LandingSDF = VF_EvaluatePassageLandingSDF(Position, *Landing);
            if (LandingSDF < MinLandingSDF)
            {
                MinLandingSDF = LandingSDF;
            }
        }
    }

    if (MinLandingSDF == FLT_MAX) return;

    // Keep the same smooth interior blend used by the passage op. This landing-specific writer is
    // idempotent because the MC-facing post may run after the structural op already saw the same
    // voxel. The landing's support floor is restored by the floor writer after this MC-facing pass.
    VF_ApplyPassageLandingCarving(Density, MinLandingSDF, BaseDensity, SealThickness);
}

void UVoxelStrateManager::ApplyPassageLandingAirMC(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness) const
{
    // ApplyDisturbances is deliberately an MC-space post-process and can add a bridge or ridge
    // on top of the structural passage. Reassert only landing air here; the support slab is
    // restored by ApplyPassageLandingFloorMC immediately afterwards. This stays on the same
    // thread-local passage shortlist as the hot voxel path and performs no source/topology work.
    float InternalDensity = -Density;
    ApplyPassageLandingAir(
        InternalDensity, WorldX, WorldY, WorldZ, BaseDensity, SealThickness);
    Density = -InternalDensity;
}

void UVoxelStrateManager::ApplyPassageTunnelAir(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageTunnelAir);
    const FVector Position(WorldX, WorldY, WorldZ);
    int32 PassageIndex = INDEX_NONE;
    float FloorZ = 0.0f;
    float SupportRadius = 0.0f;
    const bool bFoundWalkableAir = VF_FindWalkableTunnelAir(
        this, Position, PassageIndex, FloorZ, SupportRadius);
    if (bFoundWalkableAir)
    {
        const float AirTarget = -(BaseDensity * 2.0f + SealThickness + 4.0f);
        Density = FMath::Min(Density, AirTarget);
    }
}

void UVoxelStrateManager::ApplyPassageTunnelAirMC(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness) const
{
    // The disturbance layer uses MC polarity (negative = solid), so reuse the exact internal
    // tunnel-air operation rather than maintaining a second polarity-specific formula.
    float InternalDensity = -Density;
    ApplyPassageTunnelAir(
        InternalDensity, WorldX, WorldY, WorldZ, BaseDensity, SealThickness);
    Density = -InternalDensity;
}

void UVoxelStrateManager::ApplyPassageLandingFloorMC(
    float& Density, float WorldX, float WorldY, float WorldZ, float BaseDensity) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageLandingFloor);
    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(this, ChunkCoord);
    const FVector Position(WorldX, WorldY, WorldZ);
    for (const int32 PassageIndex : Nearby)
    {
        const FVoxelPassage& Passage = Passages[PassageIndex];
        if (!VoxelPassageGeometry::VerticalShaftConnectorAirMarker()
            && (VF_IsPassageLandingFloor(Position, Passage.UpperLanding)
                || VF_IsPassageLandingFloor(Position, Passage.LowerLanding)
                || VF_IsWalkableTunnelFloor(Passage, Position)))
        {
            // Result is in MC convention here (negative = solid). This reassertion is the
            // structural floor backstop after the optional MC-space disturbance layer.
            Density = FMath::Min(Density, -BaseDensity);
            break;
        }
    }
}

static bool VF_IsPassageRoomFloor(
    const FVector& Position, const FVoxelPassageLanding& Landing)
{
    if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y)
        || !FMath::IsFinite(Position.Z) || !FMath::IsFinite(Landing.FloorZ)
        || !FMath::IsFinite(Landing.FloorThickness)
        || Landing.FloorThickness <= 0.0f || Landing.HalfWidth <= 0.0f)
    {
        return false;
    }
    return Position.Z <= Landing.FloorZ + KINDA_SMALL_NUMBER
        && Position.Z > Landing.FloorZ - Landing.FloorThickness
        && FMath::Abs(Position.X - Landing.StandingPoint.X)
            <= FMath::Max(Landing.HalfWidth - 1.0f, 0.0f)
        && FMath::Abs(Position.Y - Landing.StandingPoint.Y)
            <= FMath::Max(Landing.HalfWidth - 1.0f, 0.0f);
}

void UVoxelStrateManager::ApplyPassageStructuralPostsMC(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageStructuralPosts);

    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(this, ChunkCoord);
    if (Nearby.Num() == 0) return;

    const FVector Position(WorldX, WorldY, WorldZ);
    const bool bSuppressFloor =
        VoxelPassageGeometry::VerticalShaftConnectorAirMarker();
    bool bAnyPassageFloor = false;
    bool bAnyRoomFloor = false;
    bool bWalkableAir = false;
    float MinLandingSDF = FLT_MAX;

    // The four old MC backstops all walked the same immutable per-chunk shortlist. Gather their
    // predicates in one pass; the writes below retain the old order (landing air, landing floor,
    // tunnel air, room floor), including the vertical-shaft floor suppression marker.
    for (const int32 PassageIndex : Nearby)
    {
        if (!Passages.IsValidIndex(PassageIndex)) continue;
        const FVoxelPassage& Passage = Passages[PassageIndex];

        if (!bSuppressFloor
            && (VF_IsPassageLandingFloor(Position, Passage.UpperLanding)
                || VF_IsPassageLandingFloor(Position, Passage.LowerLanding)
                || VF_IsWalkableTunnelFloor(Passage, Position)))
        {
            bAnyPassageFloor = true;
        }

        if (!bSuppressFloor
            && (VF_IsPassageRoomFloor(Position, Passage.UpperLanding)
                || VF_IsPassageRoomFloor(Position, Passage.LowerLanding)))
        {
            bAnyRoomFloor = true;
        }

        if (!bWalkableAir
            && FVector::DistSquared(Position, Passage.BoundCenter) <= Passage.BoundRadiusSq
            && VF_IsWalkableTunnelAir(Passage, Position))
        {
            bWalkableAir = true;
        }

        if (FVector::DistSquared(Position, Passage.BoundCenter) <= Passage.BoundRadiusSq)
        {
            const FVoxelPassageLanding* Landings[] = {
                &Passage.UpperLanding, &Passage.LowerLanding };
            for (const FVoxelPassageLanding* Landing : Landings)
            {
                MinLandingSDF = FMath::Min(
                    MinLandingSDF,
                    VF_EvaluatePassageLandingSDF(Position, *Landing));
            }
        }
    }

    if (!bAnyPassageFloor && MinLandingSDF < FLT_MAX)
    {
        float InternalDensity = -Density;
        VF_ApplyPassageLandingCarving(
            InternalDensity, MinLandingSDF, BaseDensity, SealThickness);
        Density = -InternalDensity;
    }

    if (bAnyPassageFloor)
    {
        Density = FMath::Min(Density, -BaseDensity);
    }

    if (bWalkableAir)
    {
        Density = FMath::Max(
            Density, BaseDensity * 2.0f + SealThickness + 4.0f);
    }

    if (bAnyRoomFloor)
    {
        Density = FMath::Min(Density, -BaseDensity);
    }
}

void UVoxelStrateManager::ApplyPassageLandingRoomFloorMC(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageLandingRoomFloor);
    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(this, ChunkCoord);
    const FVector Position(WorldX, WorldY, WorldZ);
    for (const int32 PassageIndex : Nearby)
    {
        const FVoxelPassage& Passage = Passages[PassageIndex];
        if (!VoxelPassageGeometry::VerticalShaftConnectorAirMarker()
            && (VF_IsPassageRoomFloor(Position, Passage.UpperLanding)
                || VF_IsPassageRoomFloor(Position, Passage.LowerLanding)))
        {
            Density = FMath::Min(Density, -BaseDensity);
            break;
        }
    }
}

bool UVoxelStrateManager::AnyPassageNearBox(const FVector& MinVoxel, const FVector& MaxVoxel) const
{
    // Le carve d'un passage atteint ModSDF < PASSAGE_BLEND_RADIUS (4, VoxelGenerator.cpp) au-delà de
    // sa surface ; BoundRadius inclut déjà rayon + blend, on re-pad par sécurité (conservatif).
    constexpr float CarvePad = 4.0f;
    for (const FVoxelPassage& P : Passages)
    {
        // Point de la boîte le plus proche du centre de la sphère → test sphère/AABB.
        const FVector C(
            FMath::Clamp(P.BoundCenter.X, MinVoxel.X, MaxVoxel.X),
            FMath::Clamp(P.BoundCenter.Y, MinVoxel.Y, MaxVoxel.Y),
            FMath::Clamp(P.BoundCenter.Z, MinVoxel.Z, MaxVoxel.Z));
        const float Reach = P.BoundRadius + CarvePad;
        if (FVector::DistSquared(C, P.BoundCenter) <= Reach * Reach)
        {
            return true;
        }
    }
    return false;
}

bool UVoxelStrateManager::AnyPassageLandingFloorNearBox(
    const FVector& MinVoxel, const FVector& MaxVoxel) const
{
    // A conservative AABB test is enough for the floor's only solid write. False positives cost
    // a tile; a false negative could classify an air tile uniformly and remove the player's floor.
    for (const FVoxelPassage& Passage : Passages)
    {
        const FVoxelPassageLanding* Landings[] = {
            &Passage.UpperLanding, &Passage.LowerLanding };
        for (const FVoxelPassageLanding* Landing : Landings)
        {
            if (Landing->HalfWidth <= 0.0f || Landing->FloorThickness <= 0.0f)
            {
                continue;
            }
            const float Pad = 1.0f;
            const float FloorMinX = Landing->StandingPoint.X - Landing->HalfWidth - Pad;
            const float FloorMaxX = Landing->StandingPoint.X + Landing->HalfWidth + Pad;
            const float FloorMinY = Landing->StandingPoint.Y - Landing->HalfWidth - Pad;
            const float FloorMaxY = Landing->StandingPoint.Y + Landing->HalfWidth + Pad;
            const float FloorMinZ = Landing->FloorZ - Landing->FloorThickness - Pad;
            const float FloorMaxZ = Landing->FloorZ + Pad;
            if (FloorMaxX >= MinVoxel.X && FloorMinX <= MaxVoxel.X
                && FloorMaxY >= MinVoxel.Y && FloorMinY <= MaxVoxel.Y
                && FloorMaxZ >= MinVoxel.Z && FloorMinZ <= MaxVoxel.Z)
            {
                return true;
            }

        }

        // The default tube also owns a three-voxel support band. Its conservative segment AABB
        // keeps a classifier from proving AllAir over the floor and then dropping that support
        // during meshing. False positives are intentional; false negatives would be a hole.
        if (Passage.bWalkableTunnelContract
            && Passage.ControlPoints.Num() >= 2
            && Passage.ControlRadii.Num() == Passage.ControlPoints.Num())
        {
            constexpr float Pad = 1.0f;
            for (int32 SegmentIndex = 0;
                 SegmentIndex + 1 < Passage.ControlPoints.Num();
                 ++SegmentIndex)
            {
                const FVector& A = Passage.ControlPoints[SegmentIndex];
                const FVector& B = Passage.ControlPoints[SegmentIndex + 1];
                const float SupportRadius = FMath::Max(
                    FMath::Min(
                        FMath::Abs(Passage.ControlRadii[SegmentIndex]),
                        FMath::Abs(Passage.ControlRadii[SegmentIndex + 1])) - 0.5f,
                    VoxelPassageGeometry::PlayerRadiusVoxels);
                const float FloorMinZ = FMath::Min(
                    VoxelPassageGeometry::TunnelFloorZ(
                        A, Passage.ControlRadii[SegmentIndex]),
                    VoxelPassageGeometry::TunnelFloorZ(
                        B, Passage.ControlRadii[SegmentIndex + 1]))
                    - VoxelPassageGeometry::LandingFloorThicknessVoxels - Pad;
                const float FloorMaxZ = FMath::Max(
                    VoxelPassageGeometry::TunnelFloorZ(
                        A, Passage.ControlRadii[SegmentIndex]),
                    VoxelPassageGeometry::TunnelFloorZ(
                        B, Passage.ControlRadii[SegmentIndex + 1])) + Pad;
                const float FloorMinX = FMath::Min(A.X, B.X) - SupportRadius - Pad;
                const float FloorMaxX = FMath::Max(A.X, B.X) + SupportRadius + Pad;
                const float FloorMinY = FMath::Min(A.Y, B.Y) - SupportRadius - Pad;
                const float FloorMaxY = FMath::Max(A.Y, B.Y) + SupportRadius + Pad;
                if (FloorMaxX >= MinVoxel.X && FloorMinX <= MaxVoxel.X
                    && FloorMaxY >= MinVoxel.Y && FloorMinY <= MaxVoxel.Y
                    && FloorMaxZ >= MinVoxel.Z && FloorMinZ <= MaxVoxel.Z)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

namespace
{
    static bool VF_LatticeAxisRange(
        float Min, float Max, float Origin, int32 Step,
        int32& OutFirst, int32& OutLast)
    {
        if (Step <= 0 || !FMath::IsFinite(Min) || !FMath::IsFinite(Max)
            || !FMath::IsFinite(Origin) || Min > Max)
        {
            return false;
        }
        const float InvStep = 1.0f / static_cast<float>(Step);
        OutFirst = FMath::CeilToInt((Min - Origin) * InvStep - 1.0e-4f);
        OutLast  = FMath::FloorToInt((Max - Origin) * InvStep + 1.0e-4f);
        return OutFirst <= OutLast;
    }

    static bool VF_LatticeAxisNearestDistance(
        float Min, float Max, float Origin, int32 Step, float Target, float& OutDistance)
    {
        int32 First = 0, Last = -1;
        if (!VF_LatticeAxisRange(Min, Max, Origin, Step, First, Last))
        {
            return false;
        }
        const int32 Nearest = FMath::Clamp(
            FMath::RoundToInt((Target - Origin) / static_cast<float>(Step)), First, Last);
        OutDistance = FMath::Abs(
            Origin + static_cast<float>(Nearest * Step) - Target);
        return true;
    }

    static bool VF_LatticeBoxTouchesSphere(
        const FBox& Box, const FIntVector& Origin, int32 Step,
        const FVector& Center, float Radius)
    {
        float DX = 0.0f, DY = 0.0f, DZ = 0.0f;
        if (!VF_LatticeAxisNearestDistance(
                (float)Box.Min.X, (float)Box.Max.X, (float)Origin.X,
                Step, (float)Center.X, DX)
            || !VF_LatticeAxisNearestDistance(
                (float)Box.Min.Y, (float)Box.Max.Y, (float)Origin.Y,
                Step, (float)Center.Y, DY)
            || !VF_LatticeAxisNearestDistance(
                (float)Box.Min.Z, (float)Box.Max.Z, (float)Origin.Z,
                Step, (float)Center.Z, DZ))
        {
            return false;
        }
        return DX * DX + DY * DY + DZ * DZ <= FMath::Square(Radius);
    }

    static bool VF_AnyPassageBoundTouchesLattice(
        const TArray<FVoxelPassage>& Passages,
        const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step)
    {
        if (Step <= 0 || !VoxelBox.IsValid)
        {
            // An invalid query must never manufacture an identity proof.
            return true;
        }

        for (const FVoxelPassage& Passage : Passages)
        {
            const float Reach = Passage.BoundRadius + 4.0f;
            if (!FMath::IsFinite(Reach) || Reach < 0.0f)
            {
                // Invalid generated geometry is an unknown, not an empty passage set.
                return true;
            }
            if (VF_LatticeBoxTouchesSphere(
                    VoxelBox, LatticeOrigin, Step, Passage.BoundCenter, Reach))
            {
                return true;
            }
        }
        return false;
    }

    // MaxPassageCarveFactorNearLattice is queried once for the root and once for every refined
    // child.  The child lattice is a strict subset of the root lattice, so retaining the exact
    // carve factor at each root point is a sound memo: a child reads the same values the old
    // implementation would have recomputed, in the same deterministic order.  The cache is
    // worker-local because classifier calls can run concurrently and no mutable state may be
    // published through the immutable strate manager.
    struct FPassageLatticeBoundCache
    {
        const UVoxelStrateManager* Owner = nullptr;
        uint64 OwnerLifetimeId = 0;
        uint32 Version = 0xFFFFFFFFu;
        FIntVector Origin = FIntVector::ZeroValue;
        int32 Step = 0;
        int32 FirstX = 0, LastX = -1;
        int32 FirstY = 0, LastY = -1;
        int32 FirstZ = 0, LastZ = -1;
        bool bValid = false;
        bool bNoCandidate = false;
        TArray<float> Factors;

        void Reset()
        {
            Owner = nullptr;
            OwnerLifetimeId = 0;
            Version = 0xFFFFFFFFu;
            Origin = FIntVector::ZeroValue;
            Step = 0;
            FirstX = 0; LastX = -1;
            FirstY = 0; LastY = -1;
            FirstZ = 0; LastZ = -1;
            bValid = false;
            bNoCandidate = false;
            Factors.Reset();
        }

        void Begin(const UVoxelStrateManager* InOwner, uint64 InLifetimeId,
                   uint32 InVersion, const FIntVector& InOrigin, int32 InStep,
                   int32 InFirstX, int32 InLastX,
                   int32 InFirstY, int32 InLastY,
                   int32 InFirstZ, int32 InLastZ,
                   bool bInNoCandidate)
        {
            Owner = InOwner;
            OwnerLifetimeId = InLifetimeId;
            Version = InVersion;
            Origin = InOrigin;
            Step = InStep;
            FirstX = InFirstX; LastX = InLastX;
            FirstY = InFirstY; LastY = InLastY;
            FirstZ = InFirstZ; LastZ = InLastZ;
            bValid = true;
            bNoCandidate = bInNoCandidate;
            Factors.Reset();
        }

        bool Contains(const UVoxelStrateManager* InOwner, uint64 InLifetimeId,
                      uint32 InVersion, const FIntVector& InOrigin, int32 InStep,
                      int32 InFirstX, int32 InLastX,
                      int32 InFirstY, int32 InLastY,
                      int32 InFirstZ, int32 InLastZ) const
        {
            return bValid && Owner == InOwner && OwnerLifetimeId == InLifetimeId
                && Version == InVersion && Origin == InOrigin && Step == InStep
                && InFirstX >= FirstX && InLastX <= LastX
                && InFirstY >= FirstY && InLastY <= LastY
                && InFirstZ >= FirstZ && InLastZ <= LastZ;
        }

        int32 Index(int32 IX, int32 IY, int32 IZ) const
        {
            const int32 DimX = LastX - FirstX + 1;
            const int32 DimY = LastY - FirstY + 1;
            return ((IZ - FirstZ) * DimY + (IY - FirstY)) * DimX + (IX - FirstX);
        }

        float MaxIn(const int32 InFirstX, const int32 InLastX,
                    const int32 InFirstY, const int32 InLastY,
                    const int32 InFirstZ, const int32 InLastZ) const
        {
            if (bNoCandidate) { return 0.0f; }

            float Result = 0.0f;
            for (int32 IZ = InFirstZ; IZ <= InLastZ; ++IZ)
            {
                for (int32 IY = InFirstY; IY <= InLastY; ++IY)
                {
                    for (int32 IX = InFirstX; IX <= InLastX; ++IX)
                    {
                        Result = FMath::Max(Result, Factors[Index(IX, IY, IZ)]);
                        if (Result >= 1.0f)
                        {
                            return Result;
                        }
                    }
                }
            }
            return Result;
        }
    };

    FPassageLatticeBoundCache& VF_GetPassageLatticeBoundCache()
    {
        thread_local FPassageLatticeBoundCache Cache;
        return Cache;
    }

    static bool VF_LatticeBoxTouchesAABB(
        const FBox& Box, const FIntVector& Origin, int32 Step,
        float MinX, float MaxX, float MinY, float MaxY, float MinZ, float MaxZ)
    {
        return VoxelPassageGeometry::LatticeAxisHasSampleInInterval(
                   FMath::Max((float)Box.Min.X, MinX),
                   FMath::Min((float)Box.Max.X, MaxX),
                   (float)Origin.X, Step)
            && VoxelPassageGeometry::LatticeAxisHasSampleInInterval(
                   FMath::Max((float)Box.Min.Y, MinY),
                   FMath::Min((float)Box.Max.Y, MaxY),
                   (float)Origin.Y, Step)
            && VoxelPassageGeometry::LatticeAxisHasSampleInInterval(
                   FMath::Max((float)Box.Min.Z, MinZ),
                   FMath::Min((float)Box.Max.Z, MaxZ),
                   (float)Origin.Z, Step);
    }
}

bool UVoxelStrateManager::AnyPassageNearLattice(
    const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step) const
{
    // This is deliberately only the conservative spatial candidate test.  The old implementation
    // evaluated the complete modifier SDF here and then evaluated it a second time in
    // MaxPassageCarveFactorNearLattice during the same fold.  A bound hit is sufficient to keep
    // the carve hypothesis alive; MaxPassageCarveFactorNearLattice supplies the exact lattice
    // amplitude before the fold can preserve AllSolid.
    return VF_AnyPassageBoundTouchesLattice(
        Passages, VoxelBox, LatticeOrigin, Step);
}

float UVoxelStrateManager::MaxPassageCarveFactorNearLattice(
    const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step) const
{
    // The passage bound is only a cheap candidate test.  It encloses an entire multi-segment
    // passage, so using it as the final lattice proof turns a long tunnel into a false positive
    // for every tile inside its enclosing sphere.  That is particularly damaging at LOD0: the
    // classifier then falls through to a full density grid even when every sampled point is rock.
    //
    // Once a bound reaches this lattice, ask the same exact modifier SDF used by
    // ApplyPassageCarvingOnly at the actual MC samples.  This is still a proof, not a sampling
    // shortcut: the operator is evaluated at precisely the points the mesher will evaluate, and
    // a non-finite result remains conservative.  The common no-candidate path stays O(P), while a
    // false-positive bound pays one exact lattice walk and can then be classified as Identity.
    if (Step <= 0 || !VoxelBox.IsValid)
    {
        return 1.0f;
    }

    int32 FirstX = 0, LastX = -1;
    int32 FirstY = 0, LastY = -1;
    int32 FirstZ = 0, LastZ = -1;
    if (!VF_LatticeAxisRange(
            VoxelBox.Min.X, VoxelBox.Max.X, (float)LatticeOrigin.X, Step, FirstX, LastX)
        || !VF_LatticeAxisRange(
            VoxelBox.Min.Y, VoxelBox.Max.Y, (float)LatticeOrigin.Y, Step, FirstY, LastY)
        || !VF_LatticeAxisRange(
            VoxelBox.Min.Z, VoxelBox.Max.Z, (float)LatticeOrigin.Z, Step, FirstZ, LastZ))
    {
        return 1.0f;
    }

    FPassageLatticeBoundCache& Cache = VF_GetPassageLatticeBoundCache();
    const uint64 LifetimeId = GetCacheLifetimeId();
    const uint32 Version = GetLayoutVersion();
    if (Cache.Contains(
            this, LifetimeId, Version, LatticeOrigin, Step,
            FirstX, LastX, FirstY, LastY, FirstZ, LastZ))
    {
        return Cache.MaxIn(FirstX, LastX, FirstY, LastY, FirstZ, LastZ);
    }

    if (!VF_AnyPassageBoundTouchesLattice(
            Passages, VoxelBox, LatticeOrigin, Step))
    {
        Cache.Begin(
            this, LifetimeId, Version, LatticeOrigin, Step,
            FirstX, LastX, FirstY, LastY, FirstZ, LastZ,
            /*bInNoCandidate*/ true);
        return 0.0f;
    }

    constexpr float PassageCarveThreshold = 4.0f;
    auto CarveFactorFromSDF = [](float ModifierSDF)
    {
        if (!(ModifierSDF < PassageCarveThreshold))
        {
            return 0.0f;
        }
        float CarveFactor = FMath::Clamp(
            (PassageCarveThreshold - ModifierSDF)
                / (PassageCarveThreshold * 2.0f),
            0.0f, 1.0f);
        return SmoothStep01(CarveFactor);
    };

    const int64 DimX = static_cast<int64>(LastX) - FirstX + 1;
    const int64 DimY = static_cast<int64>(LastY) - FirstY + 1;
    const int64 DimZ = static_cast<int64>(LastZ) - FirstZ + 1;
    const int64 SampleCount = DimX * DimY * DimZ;
    constexpr int64 MaxCachedLatticeSamples = 65536;
    if (SampleCount > 0 && SampleCount <= MaxCachedLatticeSamples)
    {
        Cache.Begin(
            this, LifetimeId, Version, LatticeOrigin, Step,
            FirstX, LastX, FirstY, LastY, FirstZ, LastZ,
            /*bInNoCandidate*/ false);
        Cache.Factors.SetNumUninitialized(static_cast<int32>(SampleCount));

        float MaxCarveFactor = 0.0f;
        for (int32 IZ = FirstZ; IZ <= LastZ; ++IZ)
        {
            const float Z = (float)LatticeOrigin.Z + (float)(IZ * Step);
            for (int32 IY = FirstY; IY <= LastY; ++IY)
            {
                const float Y = (float)LatticeOrigin.Y + (float)(IY * Step);
                for (int32 IX = FirstX; IX <= LastX; ++IX)
                {
                    const float X = (float)LatticeOrigin.X + (float)(IX * Step);
                    const float ModifierSDF = EvaluateModifierSDF(X, Y, Z);
                    const float CarveFactor = FMath::IsFinite(ModifierSDF)
                        ? CarveFactorFromSDF(ModifierSDF) : 1.0f;
                    Cache.Factors[Cache.Index(IX, IY, IZ)] = CarveFactor;
                    MaxCarveFactor = FMath::Max(MaxCarveFactor, CarveFactor);
                    if (MaxCarveFactor >= 1.0f)
                    {
                        // The remaining values are still filled below. Descendant queries need
                        // the complete exact lattice, not merely this query's maximum.
                        for (int32 RemainingZ = IZ; RemainingZ <= LastZ; ++RemainingZ)
                        {
                            const float RemainingWorldZ =
                                (float)LatticeOrigin.Z + (float)(RemainingZ * Step);
                            const int32 StartY = RemainingZ == IZ ? IY : FirstY;
                            for (int32 RemainingY = StartY; RemainingY <= LastY; ++RemainingY)
                            {
                                const float RemainingWorldY =
                                    (float)LatticeOrigin.Y + (float)(RemainingY * Step);
                                const int32 StartX =
                                    (RemainingZ == IZ && RemainingY == IY) ? IX + 1 : FirstX;
                                for (int32 RemainingX = StartX; RemainingX <= LastX; ++RemainingX)
                                {
                                    const float RemainingWorldX =
                                        (float)LatticeOrigin.X + (float)(RemainingX * Step);
                                    const float RemainingSDF = EvaluateModifierSDF(
                                        RemainingWorldX, RemainingWorldY, RemainingWorldZ);
                                    Cache.Factors[Cache.Index(RemainingX, RemainingY, RemainingZ)] =
                                        FMath::IsFinite(RemainingSDF)
                                            ? CarveFactorFromSDF(RemainingSDF) : 1.0f;
                                }
                            }
                        }
                        return MaxCarveFactor;
                    }
                }
            }
        }
        return MaxCarveFactor;
    }

    // Very large or unusual external queries do not enter the bounded worker memo. Keep the
    // original exact walk for them; refusing the cache must never change the proof.
    Cache.Reset();
    float MaxCarveFactor = 0.0f;
    for (int32 IZ = FirstZ; IZ <= LastZ; ++IZ)
    {
        const float Z = (float)LatticeOrigin.Z + (float)(IZ * Step);
        for (int32 IY = FirstY; IY <= LastY; ++IY)
        {
            const float Y = (float)LatticeOrigin.Y + (float)(IY * Step);
            for (int32 IX = FirstX; IX <= LastX; ++IX)
            {
                const float X = (float)LatticeOrigin.X + (float)(IX * Step);
                const float ModifierSDF = EvaluateModifierSDF(X, Y, Z);
                if (!FMath::IsFinite(ModifierSDF))
                {
                    return 1.0f;
                }
                MaxCarveFactor = FMath::Max(
                    MaxCarveFactor, CarveFactorFromSDF(ModifierSDF));
                if (MaxCarveFactor >= 1.0f)
                {
                    return MaxCarveFactor;
                }
            }
        }
    }
    return MaxCarveFactor;
}

bool UVoxelStrateManager::AnyPassageLandingFloorNearLattice(
    const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step) const
{
    for (const FVoxelPassage& Passage : Passages)
    {
        const FVoxelPassageLanding* Landings[] = {
            &Passage.UpperLanding, &Passage.LowerLanding };
        for (const FVoxelPassageLanding* Landing : Landings)
        {
            if (Landing->HalfWidth <= 0.0f || Landing->FloorThickness <= 0.0f)
            {
                continue;
            }
            constexpr float Pad = 1.0f;
            if (VF_LatticeBoxTouchesAABB(
                    VoxelBox, LatticeOrigin, Step,
                    Landing->StandingPoint.X - Landing->HalfWidth - Pad,
                    Landing->StandingPoint.X + Landing->HalfWidth + Pad,
                    Landing->StandingPoint.Y - Landing->HalfWidth - Pad,
                    Landing->StandingPoint.Y + Landing->HalfWidth + Pad,
                    Landing->FloorZ - Landing->FloorThickness - Pad,
                    Landing->FloorZ + Pad))
            {
                return true;
            }
        }

        if (Passage.bWalkableTunnelContract
            && Passage.ControlPoints.Num() >= 2
            && Passage.ControlRadii.Num() == Passage.ControlPoints.Num())
        {
            constexpr float Pad = 1.0f;
            for (int32 SegmentIndex = 0;
                 SegmentIndex + 1 < Passage.ControlPoints.Num(); ++SegmentIndex)
            {
                const FVector& A = Passage.ControlPoints[SegmentIndex];
                const FVector& B = Passage.ControlPoints[SegmentIndex + 1];
                const float SupportRadius = FMath::Max(
                    FMath::Min(
                        FMath::Abs(Passage.ControlRadii[SegmentIndex]),
                        FMath::Abs(Passage.ControlRadii[SegmentIndex + 1])) - 0.5f,
                    VoxelPassageGeometry::PlayerRadiusVoxels);
                const float FloorA = VoxelPassageGeometry::TunnelFloorZ(
                    A, Passage.ControlRadii[SegmentIndex]);
                const float FloorB = VoxelPassageGeometry::TunnelFloorZ(
                    B, Passage.ControlRadii[SegmentIndex + 1]);
                if (VF_LatticeBoxTouchesAABB(
                        VoxelBox, LatticeOrigin, Step,
                        FMath::Min(A.X, B.X) - SupportRadius - Pad,
                        FMath::Max(A.X, B.X) + SupportRadius + Pad,
                        FMath::Min(A.Y, B.Y) - SupportRadius - Pad,
                        FMath::Max(A.Y, B.Y) + SupportRadius + Pad,
                        FMath::Min(FloorA, FloorB)
                            - VoxelPassageGeometry::LandingFloorThicknessVoxels - Pad,
                        FMath::Max(FloorA, FloorB) + Pad))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool UVoxelStrateManager::AnyOriginLandingNearBox(
    const FVector& MinVoxel, const FVector& MaxVoxel) const
{
    if (OriginSpineRadius <= 0.0f) return false;
    const FBox VoxelBox(MinVoxel, MaxVoxel);
    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (Slot.Definition == nullptr) continue;
        const float TopZ = (static_cast<float>(Slot.TopChunkZ) + 1.0f) * CHUNK_SIZE;
        const float BottomZ = static_cast<float>(Slot.BottomChunkZ) * CHUNK_SIZE;
        if (VoxelPassageGeometry::OriginLandingRoomTouchesBox(
                VoxelBox, TopZ, BottomZ,
                VF_BoundarySealThicknessForDefinition(*Slot.Definition),
                OriginSpineRadius))
        {
            return true;
        }
    }
    return false;
}

bool UVoxelStrateManager::AnyOriginLandingFloorNearBox(
    const FVector& MinVoxel, const FVector& MaxVoxel) const
{
    if (OriginSpineRadius <= 0.0f) return false;
    const FBox VoxelBox(MinVoxel, MaxVoxel);
    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (Slot.Definition == nullptr) continue;
        const float TopZ = (static_cast<float>(Slot.TopChunkZ) + 1.0f) * CHUNK_SIZE;
        const float BottomZ = static_cast<float>(Slot.BottomChunkZ) * CHUNK_SIZE;
        if (VoxelPassageGeometry::OriginLandingFloorTouchesBox(
                VoxelBox, TopZ, BottomZ,
                VF_BoundarySealThicknessForDefinition(*Slot.Definition),
                OriginSpineRadius))
        {
            return true;
        }
    }
    return false;
}

bool UVoxelStrateManager::AnyOriginLandingNearLattice(
    const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step) const
{
    if (OriginSpineRadius <= 0.0f) return false;
    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (Slot.Definition == nullptr) continue;
        const float TopZ = (static_cast<float>(Slot.TopChunkZ) + 1.0f) * CHUNK_SIZE;
        const float BottomZ = static_cast<float>(Slot.BottomChunkZ) * CHUNK_SIZE;
        const float Seal = VF_BoundarySealThicknessForDefinition(*Slot.Definition);
        if (VoxelPassageGeometry::OriginLandingRoomTouchesLattice(
                VoxelBox, LatticeOrigin, Step, TopZ, BottomZ, Seal, OriginSpineRadius))
        {
            return true;
        }
    }
    return false;
}

bool UVoxelStrateManager::AnyOriginLandingFloorNearLattice(
    const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step) const
{
    if (OriginSpineRadius <= 0.0f) return false;
    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (Slot.Definition == nullptr) continue;
        const float TopZ = (static_cast<float>(Slot.TopChunkZ) + 1.0f) * CHUNK_SIZE;
        const float BottomZ = static_cast<float>(Slot.BottomChunkZ) * CHUNK_SIZE;
        const float Seal = VF_BoundarySealThicknessForDefinition(*Slot.Definition);
        if (VoxelPassageGeometry::OriginLandingFloorTouchesLattice(
                VoxelBox, LatticeOrigin, Step, TopZ, BottomZ, Seal, OriginSpineRadius))
        {
            return true;
        }
    }
    return false;
}

//=============================================================================
// QUERIES
//=============================================================================

int32 UVoxelStrateManager::FindSlotIndexForChunkZ(int32 ChunkZ) const
{
    // Linear search through strate layout.
    // With ~10-20 strates this is fine. If we ever have hundreds,
    // switch to binary search (layout is sorted by Z).
    for (int32 i = 0; i < StrateLayout.Num(); i++)
    {
        const FStrateSlot& Slot = StrateLayout[i];
        if (ChunkZ <= Slot.TopChunkZ && ChunkZ >= Slot.BottomChunkZ)
        {
            return i;
        }
    }
    return -1;
}

UVoxelStrateDefinition* UVoxelStrateManager::GetStrateAt(float WorldZ) const
{
    // Convert world Z to chunk Z coordinate
    int32 ChunkZ = FMath::FloorToInt((WorldZ / VOXEL_SIZE) / CHUNK_SIZE);
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkZ);
    if (SlotIdx >= 0)
    {
        return StrateLayout[SlotIdx].Definition;
    }
    return nullptr;
}

int32 UVoxelStrateManager::GetStrateIndex(float WorldZ) const
{
    int32 ChunkZ = FMath::FloorToInt((WorldZ / VOXEL_SIZE) / CHUNK_SIZE);
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkZ);
    if (SlotIdx >= 0)
    {
        return StrateLayout[SlotIdx].StrateIndex;
    }
    return -1;
}

UVoxelStrateDefinition* UVoxelStrateManager::GetStrateForChunk(const FIntVector& ChunkCoord) const
{
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx >= 0)
    {
        return StrateLayout[SlotIdx].Definition;
    }
    return nullptr;
}

bool UVoxelStrateManager::GetStrateChunkZBounds(int32 ChunkZ, int32& OutTopChunkZ, int32& OutBottomChunkZ) const
{
    // Strate-aware vertical streaming. Returns the chunk-Z span of the strate containing
    // ChunkZ; false if ChunkZ is in the inter-strate gap (or outside the layout) — there the
    // caller leaves the vertical view UNCLAMPED, since the gap is a brief see-both-sides
    // descent transition. TopChunkZ > BottomChunkZ (Z decreases downward).
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkZ);
    if (SlotIdx < 0)
    {
        return false;
    }
    OutTopChunkZ    = StrateLayout[SlotIdx].TopChunkZ;
    OutBottomChunkZ = StrateLayout[SlotIdx].BottomChunkZ;
    return true;
}

ECaveGeneratorType UVoxelStrateManager::GetGeneratorTypeForChunk(const FIntVector& ChunkCoord) const
{
    // Look up which slot this chunk falls into.
    // If outside all strates (above or below), default to TunnelNetwork —
    // the fallback density path will produce solid rock anyway.
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        return ECaveGeneratorType::TunnelNetwork;
    }

#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override =
        FindComposerOverride(StrateLayout[SlotIdx].StrateIndex))
    {
        return Override->Archetype;
    }
#endif

    if (SeasonStrates.IsValidIndex(SlotIdx))
    {
        return SeasonStrates[SlotIdx].Archetype;
    }

    return StrateLayout[SlotIdx].Definition->GeneratorType;
}

bool UVoxelStrateManager::UsesOperatorStackForChunk(const FIntVector& ChunkCoord) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition) { return false; }

    if (SeasonStrates.IsValidIndex(SlotIdx))
    {
        // A recipe is an explicit stack opt-in. Native fixed entries retain the authored/switch
        // route while still reading their season parameter vector.
        return SeasonStrates[SlotIdx].bUsesRecipe;
    }

#if WITH_EDITOR
    if (FindComposerOverride(StrateLayout[SlotIdx].StrateIndex) != nullptr)
    {
        // Temporary composer candidates are measured through the stack path. Do not inherit an
        // unrelated asset flag from the slot being replaced.
        return true;
    }
#endif

    const UVoxelStrateDefinition* Def = StrateLayout[SlotIdx].Definition;
    if (!Def->bUseOperatorStack) { return false; }

    // LA LISTE DES ARCHÉTYPES PORTÉS — le seul endroit où elle est écrite. Un archétype non porté
    // ignore le drapeau et retombe sur le `switch`, pour qu'on puisse cocher la case sur n'importe
    // quelle strate sans rien casser en attendant son portage.
    // THE PORTED-ARCHETYPE LIST, written down exactly once. An unported archetype ignores the flag
    // and falls back to the switch, so the box can be ticked anywhere without breaking anything.
    switch (Def->GeneratorType)
    {
    case ECaveGeneratorType::Maze:            return true;   // Phase 1
    case ECaveGeneratorType::FlatPlain:                      // Phase 2 — les deux partagent
    case ECaveGeneratorType::CrystalChamber:  return true;   //   UNE seule pile (BuildSlabStack)

    case ECaveGeneratorType::SurfaceWorld:
        // ✅ La garde « pas de biomes » est TOMBÉE (étape 2c) : le combiner `Mask` existe, donc une
        // strate à biomes mélange bien ses hauteurs comme le chemin d'origine. Les trois archétypes
        // du dessus plus celui-ci font 5 des 8 portés.
        // The no-biome guard is GONE: the Mask combiner exists, so a biome strate blends its heights
        // exactly as the original path does.
        return true;

    case ECaveGeneratorType::VerticalShafts:  return true;   // Phase 2 — 3 ops repris de Maze tels quels

    case ECaveGeneratorType::FloatingIslands:
        // Phase 2 — la pile qui tourne à l'ENVERS : source de VIDE + fill, au lieu de source de ROC
        // + carve, avec les MÊMES opérateurs au signe près.
        return true;

    case ECaveGeneratorType::Underwater:
        // ⚠️ AUCUNE PILE À ELLE : `Underwater` EST `TunnelNetwork` plus un drapeau d'eau consommé
        // côté rendu. `GetDensityAt` les met dans le même `case`, et `WaterLevelRelative` n'est lu
        // que par `GetWaterLevel*` de ce manager — jamais par la densité (vérifié, pas supposé).
    case ECaveGeneratorType::TunnelNetwork:
        // Phase 2, LE DERNIER, et le plus gros : ~1080 lignes portées en trois étapes (squelette
        // SDF → douze modificateurs de détail → override d'op par salle), 19 opérateurs, dont
        // `FRoomGraphSource` qui **APPELLE** `BuildChunkCache`/`EvaluateSDFCached` au lieu de les
        // transcrire — c'est là que vit la discipline d'invariance de fenêtre d'ARCHITECTURE §8.4,
        // et en forker une copie aurait été le pire résultat possible de ce refactor.
        //
        // **8 SUR 8.** Le `switch` d'archétypes a désormais un jumeau en pile d'opérateurs, opt-in
        // par strate, chacun vérifié par un test d'équivalence bit à bit contre sa fonction
        // d'origine. Ce qui n'est PAS fait : `ClassifyTile` n'utilise toujours pas `ClassifyBox`.
        return true;

    default:                                  return false;
    }
}

bool UVoxelStrateManager::IsGapChunk(const FIntVector& ChunkCoord) const
{
    if (StrateLayout.Num() == 0) return false;

    // Above the top strate or below the bottom strate = open air, NOT a gap.
    const int32 StackTop    = StrateLayout[0].TopChunkZ;
    const int32 StackBottom = StrateLayout.Last().BottomChunkZ;
    if (ChunkCoord.Z > StackTop || ChunkCoord.Z < StackBottom) return false;

    // Inside the stack's Z span but not in any strate slot → it's a bedrock gap.
    return FindSlotIndexForChunkZ(ChunkCoord.Z) < 0;
}

FSlabGenerationParams UVoxelStrateManager::GetSlabParamsForChunk(const FIntVector& ChunkCoord) const
{
    // Fallback: empty params with BaseDensity < 0 → all-air outside strate range.
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        FSlabGenerationParams Empty;
        Empty.BaseDensity = -1.0f;
        return Empty;
    }

    const FStrateSlot& Slot = StrateLayout[SlotIdx];

    // Copy the designer-authored slab params from the strate definition, unless an editor
    // composer candidate owns this slot.
    FSlabGenerationParams Result;
    if (SeasonStrates.IsValidIndex(SlotIdx))
    {
        Result = SeasonStrates[SlotIdx].Params.SlabParams;
    }
#if WITH_EDITOR
    else if (const FVoxelStrateComposerSlotOverride* Override =
        FindComposerOverride(Slot.StrateIndex))
    {
        Result = (Override->Archetype == ECaveGeneratorType::FlatPlain
                  || Override->Archetype == ECaveGeneratorType::CrystalChamber)
            ? Override->Params.SlabParams
            : Slot.Definition->SlabParams;
    }
    else
#endif
    {
        Result = Slot.Definition->SlabParams;
    }

    // Fill in the runtime Z bounds (voxel coordinates, same convention as
    // FStrateGenerationParams::StrateTopWorldZ / StrateBottomWorldZ).
    // TopChunkZ+1 because the top chunk's CEILING is at (TopChunkZ+1)*CHUNK_SIZE.
    Result.StrateTopWorldZ    = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    Result.StrateBottomWorldZ = (float)(Slot.BottomChunkZ)  * CHUNK_SIZE;

    return Result;
}

//=============================================================================
// PER-ARCHETYPE PARAM GETTERS
//=============================================================================
// Each mirrors GetSlabParamsForChunk: copy designer params, fill runtime Z bounds.
// No cross-boundary blending — archetypes meet at Hard boundaries.

FMazeGenerationParams UVoxelStrateManager::GetMazeParamsForChunk(const FIntVector& ChunkCoord) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        FMazeGenerationParams Empty;
        Empty.BaseDensity = -1.0f;
        return Empty;
    }

    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    FMazeGenerationParams Result = SeasonStrates.IsValidIndex(SlotIdx)
        ? SeasonStrates[SlotIdx].Params.MazeParams : Slot.Definition->MazeParams;
#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override = FindComposerOverride(Slot.StrateIndex))
    {
        if (Override->Archetype == ECaveGeneratorType::Maze)
        {
            Result = Override->Params.MazeParams;
        }
    }
#endif
    Result.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    Result.StrateBottomWorldZ = (float)Slot.BottomChunkZ * CHUNK_SIZE;
    return Result;
}

FSurfaceGenerationParams UVoxelStrateManager::GetSurfaceParamsForChunk(const FIntVector& ChunkCoord) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        FSurfaceGenerationParams Empty;
        Empty.BaseDensity = -1.0f;
        return Empty;
    }

    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    FSurfaceGenerationParams Result = SeasonStrates.IsValidIndex(SlotIdx)
        ? SeasonStrates[SlotIdx].Params.SurfaceParams : Slot.Definition->SurfaceParams;
#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override = FindComposerOverride(Slot.StrateIndex))
    {
        if (Override->Archetype == ECaveGeneratorType::SurfaceWorld)
        {
            Result = Override->Params.SurfaceParams;
        }
    }
#endif
    Result.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    Result.StrateBottomWorldZ = (float)Slot.BottomChunkZ * CHUNK_SIZE;
    return Result;
}

FVerticalShaftParams UVoxelStrateManager::GetVerticalShaftParamsForChunk(const FIntVector& ChunkCoord) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        FVerticalShaftParams Empty;
        Empty.BaseDensity = -1.0f;
        return Empty;
    }

    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    FVerticalShaftParams Result = SeasonStrates.IsValidIndex(SlotIdx)
        ? SeasonStrates[SlotIdx].Params.VerticalShaftParams : Slot.Definition->VerticalShaftParams;
#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override = FindComposerOverride(Slot.StrateIndex))
    {
        if (Override->Archetype == ECaveGeneratorType::VerticalShafts)
        {
            Result = Override->Params.VerticalShaftParams;
        }
    }
#endif
    Result.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    Result.StrateBottomWorldZ = (float)Slot.BottomChunkZ * CHUNK_SIZE;
    return Result;
}

FFloatingIslandParams UVoxelStrateManager::GetFloatingIslandParamsForChunk(const FIntVector& ChunkCoord) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        FFloatingIslandParams Empty;
        Empty.BaseDensity = -1.0f;
        return Empty;
    }

    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    FFloatingIslandParams Result = SeasonStrates.IsValidIndex(SlotIdx)
        ? SeasonStrates[SlotIdx].Params.FloatingIslandParams : Slot.Definition->FloatingIslandParams;
#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override = FindComposerOverride(Slot.StrateIndex))
    {
        if (Override->Archetype == ECaveGeneratorType::FloatingIslands)
        {
            Result = Override->Params.FloatingIslandParams;
        }
    }
#endif
    Result.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    Result.StrateBottomWorldZ = (float)Slot.BottomChunkZ * CHUNK_SIZE;
    return Result;
}

FBiomeContext UVoxelStrateManager::GetBiomeContextForChunk(const FIntVector& ChunkCoord) const
{
    FBiomeContext Out;

    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition) return Out;

    const UVoxelStrateDefinition* Def = StrateLayout[SlotIdx].Definition;
    if (Def->Biomes.Num() == 0) return Out;   // biomes disabled for this strate

    Out.Map = Def->BiomeMapParams;
    Out.Biomes.Reserve(Def->Biomes.Num());
    for (int32 i = 0; i < Def->Biomes.Num(); ++i)
    {
        const UVoxelBiomeDefinition* B = Def->Biomes[i];
        if (!B) continue;   // skip null entries (keep original index for content lookup)

        FBiomeResolved R;
        R.Index       = i;
        R.ReliefMin   = B->ReliefMin;   R.ReliefMax   = B->ReliefMax;
        R.MoistureMin = B->MoistureMin; R.MoistureMax = B->MoistureMax;
        R.DebugColor  = B->DebugColor.ToFColor(true);
        R.MaterialPaletteIndex = B->MaterialPaletteIndex;
        Out.Biomes.Add(R);
    }
    return Out;
}

bool UVoxelStrateManager::GetStrateUnrealZRange(float WorldZ, float& OutTopZ, float& OutBottomZ) const
{
    const int32 ChunkZ = FMath::FloorToInt((WorldZ / VOXEL_SIZE) / CHUNK_SIZE);
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkZ);
    if (SlotIdx < 0) return false;

    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    // Voxel-space Z bounds → Unreal units. Ceiling = top chunk's upper edge.
    OutTopZ    = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE * VOXEL_SIZE;
    OutBottomZ = (float)(Slot.BottomChunkZ)  * CHUNK_SIZE * VOXEL_SIZE;
    return true;
}

FStrateDisturbanceParams UVoxelStrateManager::GetDisturbanceParamsForChunk(const FIntVector& ChunkCoord) const
{
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        return FStrateDisturbanceParams();  // all features disabled
    }
    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    FStrateDisturbanceParams Result = Slot.Definition->Disturbances;
    Result.StrateTopWorldZ    = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    Result.StrateBottomWorldZ = (float)(Slot.BottomChunkZ)  * CHUNK_SIZE;
    return Result;
}

float UVoxelStrateManager::GetWaterLevelWorldZForChunk(const FIntVector& ChunkCoord) const
{
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition) return -FLT_MAX;

    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    const UVoxelStrateDefinition* Def = Slot.Definition;
    if (!Def->bHasWater) return -FLT_MAX;

    // Pull the relative level from whichever archetype owns water.
    float Rel = 0.0f;
    const ECaveGeneratorType Archetype = SeasonStrates.IsValidIndex(SlotIdx)
        ? SeasonStrates[SlotIdx].Archetype : Def->GeneratorType;
    switch (Archetype)
    {
    case ECaveGeneratorType::SurfaceWorld:
        Rel = SeasonStrates.IsValidIndex(SlotIdx)
            ? SeasonStrates[SlotIdx].Params.SurfaceParams.WaterLevelRelative
            : Def->SurfaceParams.WaterLevelRelative;
        break;
    default:
        Rel = SeasonStrates.IsValidIndex(SlotIdx)
            ? SeasonStrates[SlotIdx].Params.TunnelNetworkParams.WaterLevelRelative
            : Def->GenerationParams.WaterLevelRelative;
        break;
    }
    if (Rel <= 0.0f) return -FLT_MAX;

    const float BottomZ = (float)(Slot.BottomChunkZ)  * CHUNK_SIZE;
    const float TopZ    = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    return FMath::Lerp(BottomZ, TopZ, FMath::Clamp(Rel, 0.0f, 1.0f));
}

FStrateGenerationParams UVoxelStrateManager::GetGenerationParams(const FIntVector& ChunkCoord) const
{
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);

    // If outside all strates, return negative density → guaranteed air.
    // BaseDensity must be < 0 because IsoLevel is 0.0 and density >= IsoLevel = solid.
    if (SlotIdx < 0)
    {
        FStrateGenerationParams Empty;
        Empty.BaseDensity = -1.0f;   // Negative → air after negation
        Empty.WormStrength = 0.0f;
        Empty.RoomDensity = 0.0f;    // No rooms outside strates
        return Empty;
    }

    const FStrateSlot& Slot = StrateLayout[SlotIdx];

#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override =
        FindComposerOverride(Slot.StrateIndex))
    {
        FStrateGenerationParams Result = Override->Params.TunnelNetworkParams;
        Result.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
        Result.StrateBottomWorldZ = (float)Slot.BottomChunkZ * CHUNK_SIZE;
        // A candidate is a hard replacement of this slot. Do not blend its rolled vector with an
        // authored neighbour at a boundary; this is also how the offline fixture measures it.
        return Result;
    }
#endif

    if (SeasonStrates.IsValidIndex(SlotIdx))
    {
        FStrateGenerationParams Result = SeasonStrates[SlotIdx].Params.TunnelNetworkParams;
        Result.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
        Result.StrateBottomWorldZ = (float)Slot.BottomChunkZ * CHUNK_SIZE;
        // Season vectors are already final, measured slot records. Vertical structural posts own
        // their boundary; blending them with another selected recipe would describe neither one.
        return Result;
    }

    FStrateGenerationParams BaseParams = BuildParamsFromDefinition(Slot.Definition);

    //=========================================================================
    // SET STRATE BOUNDARY Z VALUES
    //=========================================================================
    // The density function needs to know the strate's Z range (in voxel coords)
    // to seal the top and bottom with solid rock. This prevents caves from
    // carving through strate boundaries.
    //
    // TopChunkZ=0, CHUNK_SIZE=32: top of the strate = chunk 0's top edge = voxel Z=32
    // BottomChunkZ=-3: bottom of the strate = chunk -3's bottom edge = voxel Z=-3*32 = -96
    BaseParams.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    BaseParams.StrateBottomWorldZ = (float)(Slot.BottomChunkZ) * CHUNK_SIZE;

    //=========================================================================
    // BOUNDARY BLENDING (transition-type aware)
    //=========================================================================
    // If this chunk is near a strate boundary, apply the appropriate transition
    // based on the UPPER strate's TransitionType setting.
    //
    // Three transition styles:
    //
    //   GRADIENT (default):
    //     Classic linear lerp of all params across BlendChunks. Smooth,
    //     invisible boundary. Cave shape morphs gradually from one strate
    //     to the next over several chunks.
    //
    //   HARD:
    //     No blending at all — params switch instantly at the boundary.
    //     The abrupt change in density, room size, roughness, etc. creates
    //     a natural cliff, ledge, or visible material discontinuity.
    //     BlendChunks is ignored (effectively 0).
    //
    //   INTERLEAVED:
    //     3D Perlin noise warps the effective boundary Z position per XY column.
    //     Some columns transition early (fingers of the lower strate reach UP),
    //     others late (fingers of the upper strate reach DOWN). The Z frequency
    //     is intentionally low so the fingers are horizontal — wide, flat
    //     intrusions rather than vertical spikes.
    //
    // WHICH STRATE'S TRANSITION TYPE IS USED:
    // At the bottom boundary of strate N (between N and N+1), we use
    // strate N's (the upper strate's) TransitionType. This is consistent:
    // each strate definition controls what happens at its lower edge.
    // At the top boundary of strate N (between N-1 and N), we use
    // strate N-1's TransitionType (the strate above controls its lower edge).

    //---------------------------------------------------------------------
    // CHECK BOTTOM BOUNDARY (transitioning to strate below)
    //---------------------------------------------------------------------
    // DistFromBottom = how many chunks above the bottom edge of this strate.
    // When 0, we're right at the boundary. When == BlendChunks, we're at
    // the outer edge of the transition zone.
    int32 DistFromBottom = ChunkCoord.Z - Slot.BottomChunkZ;

    if (SlotIdx + 1 < StrateLayout.Num())
    {
        // The upper strate (this one) controls the transition type at its lower edge
        const EVoxelStrateTransition TransType = Slot.Definition->TransitionType;

        // Per-definition blend distance (overrides the manager's default BlendChunks)
        const int32 EffectiveBlend = Slot.Definition->TransitionBlendChunks;

        // Prepare the neighbor's params (only used for Gradient and Interleaved)
        const FStrateSlot& BelowSlot = StrateLayout[SlotIdx + 1];

        switch (TransType)
        {
        case EVoxelStrateTransition::Hard:
        {
            // HARD TRANSITION: No blending. The current strate's params apply
            // all the way to the boundary with zero transition zone.
            // The abrupt param change (different densities, room sizes, etc.)
            // creates a natural cliff or ledge — no special density boost needed.
            // We simply skip blending and fall through to the "return BaseParams" below.
            break;
        }

        case EVoxelStrateTransition::Gradient:
        {
            // GRADIENT TRANSITION: Classic smooth lerp across the blend zone.
            // Alpha goes from 0 (at the outer edge of the zone) to 1 (right at boundary).
            if (DistFromBottom < EffectiveBlend)
            {
                FStrateGenerationParams BelowParams = BuildParamsFromDefinition(BelowSlot.Definition);
                BelowParams.StrateTopWorldZ = (float)(BelowSlot.TopChunkZ + 1) * CHUNK_SIZE;
                BelowParams.StrateBottomWorldZ = (float)(BelowSlot.BottomChunkZ) * CHUNK_SIZE;

                // Linear alpha: 0 at EffectiveBlend chunks away, 1 at the boundary
                float Alpha = 1.0f - ((float)DistFromBottom / (float)EffectiveBlend);
                Alpha = FMath::Clamp(Alpha, 0.0f, 1.0f);

                return FStrateGenerationParams::Lerp(BaseParams, BelowParams, Alpha);
            }
            break;
        }

        case EVoxelStrateTransition::Interleaved:
        {
            // INTERLEAVED TRANSITION: 3D noise warps the effective boundary Z.
            //
            // Instead of a flat boundary plane, the boundary becomes a wavy 3D surface.
            // For each XY position, a Perlin noise sample offsets the boundary Z by
            // up to ±2 chunks. Where the noise pushes the boundary UP, the lower strate's
            // params appear earlier (its "fingers" reach into the upper strate). Where
            // the noise pushes DOWN, the upper strate's params persist longer.
            //
            // The Z frequency is intentionally 3x lower than XY frequency so the fingers
            // are horizontal slabs rather than vertical spikes — this matches how real
            // geological intrusions look (wide, flat, layered).
            //
            // WarpAmplitude of 2.0 means the boundary can shift ±2 chunks from its
            // true position. Combined with EffectiveBlend for the transition width,
            // we need to check a wider zone: EffectiveBlend + WarpAmplitude.
            const float WarpAmplitude = 2.0f;  // Max boundary offset in chunks
            const int32 CheckRange = EffectiveBlend + FMath::CeilToInt(WarpAmplitude);

            if (DistFromBottom < CheckRange)
            {
                FStrateGenerationParams BelowParams = BuildParamsFromDefinition(BelowSlot.Definition);
                BelowParams.StrateTopWorldZ = (float)(BelowSlot.TopChunkZ + 1) * CHUNK_SIZE;
                BelowParams.StrateBottomWorldZ = (float)(BelowSlot.BottomChunkZ) * CHUNK_SIZE;

                // Sample 3D Perlin noise to warp the boundary position.
                // XY frequency 0.15 gives medium-scale variation (~6-7 chunks per cycle).
                // Z frequency 0.05 gives slow vertical change — horizontal finger shapes.
                // CachedSeed offsets ensure each world has unique finger patterns.
                float WarpNoise = FMath::PerlinNoise3D(FVector(
                    ChunkCoord.X * 0.15f + CachedSeed * 0.01f,
                    ChunkCoord.Y * 0.15f + CachedSeed * 0.017f,
                    ChunkCoord.Z * 0.05f  // Lower Z frequency for horizontal "fingers"
                )) * VOXEL_NOISE_SCALE;

                // Offset the distance from boundary by the noise * amplitude.
                // Positive noise → boundary pushed up → lower strate appears earlier.
                // Negative noise → boundary pushed down → upper strate persists longer.
                float WarpedDist = (float)DistFromBottom + WarpNoise * WarpAmplitude;

                // Compute alpha from the warped distance (same formula as Gradient,
                // but using the noise-displaced distance instead of the true distance)
                float Alpha = 1.0f - FMath::Clamp(WarpedDist / (float)EffectiveBlend, 0.0f, 1.0f);

                // Only blend if alpha > 0 (we're inside the warped transition zone)
                if (Alpha > 0.0f)
                {
                    return FStrateGenerationParams::Lerp(BaseParams, BelowParams, Alpha);
                }
            }
            break;
        }
        }
    }

    //---------------------------------------------------------------------
    // CHECK TOP BOUNDARY (transitioning to strate above)
    //---------------------------------------------------------------------
    // Mirror logic: the ABOVE strate's TransitionType controls its lower edge,
    // which is this strate's upper edge. So we read from StrateLayout[SlotIdx-1].
    int32 DistFromTop = Slot.TopChunkZ - ChunkCoord.Z;

    if (SlotIdx > 0)
    {
        // The strate ABOVE controls the transition at its lower edge (= our upper edge)
        const FStrateSlot& AboveSlot = StrateLayout[SlotIdx - 1];
        const EVoxelStrateTransition TransType = AboveSlot.Definition->TransitionType;
        const int32 EffectiveBlend = AboveSlot.Definition->TransitionBlendChunks;

        switch (TransType)
        {
        case EVoxelStrateTransition::Hard:
        {
            // No blending — fall through to return BaseParams
            break;
        }

        case EVoxelStrateTransition::Gradient:
        {
            if (DistFromTop < EffectiveBlend)
            {
                FStrateGenerationParams AboveParams = BuildParamsFromDefinition(AboveSlot.Definition);
                AboveParams.StrateTopWorldZ = (float)(AboveSlot.TopChunkZ + 1) * CHUNK_SIZE;
                AboveParams.StrateBottomWorldZ = (float)(AboveSlot.BottomChunkZ) * CHUNK_SIZE;

                float Alpha = 1.0f - ((float)DistFromTop / (float)EffectiveBlend);
                Alpha = FMath::Clamp(Alpha, 0.0f, 1.0f);

                return FStrateGenerationParams::Lerp(BaseParams, AboveParams, Alpha);
            }
            break;
        }

        case EVoxelStrateTransition::Interleaved:
        {
            const float WarpAmplitude = 2.0f;
            const int32 CheckRange = EffectiveBlend + FMath::CeilToInt(WarpAmplitude);

            if (DistFromTop < CheckRange)
            {
                FStrateGenerationParams AboveParams = BuildParamsFromDefinition(AboveSlot.Definition);
                AboveParams.StrateTopWorldZ = (float)(AboveSlot.TopChunkZ + 1) * CHUNK_SIZE;
                AboveParams.StrateBottomWorldZ = (float)(AboveSlot.BottomChunkZ) * CHUNK_SIZE;

                // Same noise function but with a different seed offset to avoid
                // symmetry between top and bottom boundaries of adjacent strates
                float WarpNoise = FMath::PerlinNoise3D(FVector(
                    ChunkCoord.X * 0.15f + CachedSeed * 0.013f,
                    ChunkCoord.Y * 0.15f + CachedSeed * 0.023f,
                    ChunkCoord.Z * 0.05f
                )) * VOXEL_NOISE_SCALE;

                float WarpedDist = (float)DistFromTop + WarpNoise * WarpAmplitude;
                float Alpha = 1.0f - FMath::Clamp(WarpedDist / (float)EffectiveBlend, 0.0f, 1.0f);

                if (Alpha > 0.0f)
                {
                    return FStrateGenerationParams::Lerp(BaseParams, AboveParams, Alpha);
                }
            }
            break;
        }
        }
    }

    // Not near any boundary (or Hard transition) — use this strate's params directly
    return BaseParams;
}

//=============================================================================
// BUILD PARAMS FROM DEFINITION
//=============================================================================
// Returns the definition's base GenerationParams (cave shape, SDF, roughness, etc.).
//
// NOTE: Terrain op fields (TerraceStepHeight, ColumnDensity, etc.) are no longer
// merged here. They default to 0 (disabled) in the base params, and are applied
// per-room during BuildChunkCache() via FCachedRoom::RoomOp — each room hash-rolls
// one op from the strate's probability pool (FStrateTerrainOpEntry::Probability).
//
// Assets are still pre-loaded in Initialize() so BuildChunkCache can resolve
// soft pointers (Entry.Operation.Get()) without a disk read during generation.

FStrateGenerationParams UVoxelStrateManager::BuildParamsFromDefinition(const UVoxelStrateDefinition* Definition)
{
    if (!Definition) return FStrateGenerationParams();

    // Base params only — terrain op fields stay 0 until per-room assignment.
    return Definition->GenerationParams;
}
