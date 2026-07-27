// VoxelStrateManager.cpp
// Runtime strate layout generation and queries.

#include "VoxelStrateManager.h"
#include "VoxelSettings.h"
#include "VoxelTypes.h"  // For CHUNK_SIZE, VOXEL_SIZE, WorldToChunkCoord
#include "VoxelCaveMorphology.h"  // For VoxelSDF and VoxelHash
#include "VoxelTerrainOpDefinition.h"  // For UVoxelTerrainOpDefinition::ApplyTo
#include "VoxelBiomeDefinition.h"  // For UVoxelBiomeDefinition (biome context flatten)

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

void UVoxelStrateManager::Initialize(UVoxelSettings* Settings, int32 WorldSeed)
{
    if (!Settings)
    {
        UE_LOG(LogTemp, Error, TEXT("[StrateManager] No settings provided!"));
        return;
    }

    StrateLayout.Empty();

    const int32 TotalStrates = Settings->TotalStrates;

    //=========================================================================
    // STEP 1: Build shuffled pool (seed-based randomization)
    //=========================================================================
    // Copy the pool and shuffle it deterministically using the world seed.
    // Fixed strates are excluded from the shuffle — they always use their
    // assigned definition regardless of seed.

    TArray<UVoxelStrateDefinition*> ShuffledPool;
    for (const TSoftObjectPtr<UVoxelStrateDefinition>& SoftPtr : Settings->StratePool)
    {
        // Load the asset (synchronous for now — could be async later)
        UVoxelStrateDefinition* Def = SoftPtr.LoadSynchronous();
        if (Def)
        {
            ShuffledPool.Add(Def);
        }
    }

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
    for (auto& Pair : Settings->FixedStrates)
    {
        UVoxelStrateDefinition* Def = Pair.Value.LoadSynchronous();
        if (Def)
        {
            LoadedFixed.Add(Pair.Key, Def);
        }
    }

    // Current Z position (in chunks). Starts at 0 and goes downward (negative).
    int32 CurrentTopZ = 0;

    for (int32 i = 0; i < TotalStrates; i++)
    {
        FStrateSlot Slot;
        Slot.StrateIndex = i;

        // Pick definition: fixed or from pool
        UVoxelStrateDefinition** FixedDef = LoadedFixed.Find(i);
        if (FixedDef && *FixedDef)
        {
            Slot.Definition = *FixedDef;
        }
        else if (ShuffledPool.Num() > 0)
        {
            // Cycle through the pool (wraps around if more strates than pool entries)
            Slot.Definition = ShuffledPool[PoolCursor % ShuffledPool.Num()];
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

        // Move the cursor down for the next strate, leaving a solid-bedrock gap of
        // InterStrateGapChunks chunks between this strate and the next.
        CurrentTopZ = Slot.BottomChunkZ - 1 - FMath::Max(0, Settings->InterStrateGapChunks);

        StrateLayout.Add(Slot);

        UE_LOG(LogTemp, Log, TEXT("[StrateManager] Strate %d: '%s' | Z chunks [%d to %d] | %d chunks tall"),
            i,
            *Slot.Definition->StrateName.ToString(),
            Slot.TopChunkZ,
            Slot.BottomChunkZ,
            Slot.HeightInChunks);
    }

    CachedSeed = WorldSeed;
    bOpenSurfaceEntry = Settings->bOpenSurfaceEntry;
    OriginSpineRadius = Settings->OriginSpineRadius;
    InterStrateGapChunks = FMath::Max(0, Settings->InterStrateGapChunks);
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
}

//=============================================================================
// PASSAGE GENERATION
//=============================================================================

void UVoxelStrateManager::GeneratePassages()
{
    Passages.Empty();

    if (StrateLayout.Num() < 1) return;

    // Deterministic RNG from world seed
    FRandomStream Rng(CachedSeed ^ 0x50A55A6E);  // XOR with "PASSAGE" hash

    //=========================================================================
    // INTER-STRATE PASSAGES: tunnels connecting consecutive strates.
    // Each passage is randomly assigned one of 5 types, which determines
    // its shape, radius, and control point layout.
    //=========================================================================
    for (int32 i = 0; i < StrateLayout.Num() - 1; i++)
    {
        const FStrateSlot& Upper = StrateLayout[i];
        const FStrateSlot& Lower = StrateLayout[i + 1];

        // This (upper) strate's PassageConfig controls the descent tunnels to the layer
        // below. The (0,0) spine descent is separate (player-dug); these are the shortcuts.
        const UVoxelStrateDefinition* UpperDef = Upper.Definition;
        if (!UpperDef) continue;
        const FStratePassageConfig& Cfg = UpperDef->PassageConfig;

        // Upper strate floor and lower strate ceiling (differ when there's a bedrock gap).
        const float UpperBottomZ = (float)(Upper.BottomChunkZ) * CHUNK_SIZE;
        const float LowerTopZ    = (float)(Lower.TopChunkZ + 1) * CHUNK_SIZE;
        const float UpperMax = (float)Upper.HeightInChunks * CHUNK_SIZE * 0.9f;
        const float LowerMax = (float)Lower.HeightInChunks * CHUNK_SIZE * 0.9f;

        const float DistLo = FMath::Min(Cfg.DistanceMin, Cfg.DistanceMax);
        const float DistHi = FMath::Max(Cfg.DistanceMin, Cfg.DistanceMax);

        const int32 Conns = FMath::Max(0, Cfg.Connections);
        for (int32 c = 0; c < Conns; c++)
        {
            FVoxelPassage Passage;
            Passage.UpperStrateIndex = i;
            Passage.LowerStrateIndex = i + 1;

            // PLACEMENT: random angle, distance from the (0,0) spine within config range.
            const float Angle = Rng.FRandRange(0.0f, 2.0f * PI);
            const float Distance = Rng.FRandRange(DistLo, DistHi);
            const float PX = FMath::Cos(Angle) * Distance;
            const float PY = FMath::Sin(Angle) * Distance;

            // LENGTH: reach into each strate, capped to the interior.
            const float UpperReach = FMath::Min(Rng.FRandRange(Cfg.ReachMin, Cfg.ReachMax), UpperMax);
            const float LowerReach = FMath::Min(Rng.FRandRange(Cfg.ReachMin, Cfg.ReachMax), LowerMax);
            const float TopZ = UpperBottomZ + UpperReach;
            const float BottomZ = LowerTopZ - LowerReach;

            const int32 Segments = FMath::Clamp(Cfg.Segments, 1, 48);
            Passage.ControlPoints.Reset();
            Passage.ControlRadii.Reset();
            Passage.ControlPoints.Reserve(Segments + 1);
            Passage.ControlRadii.Reserve(Segments + 1);

            // WIDTH profile: mouth radius at the ends, mid radius in the centre (taper/bulge).
            auto RadiusAt = [&](float t) { return FMath::Lerp(Cfg.MouthRadius, Cfg.MidRadius, FMath::Sin(t * PI)); };

            // Per-passage shape seeds.
            const float WormFreq = Rng.FRandRange(0.8f, 1.8f);   // (vertical wobble only)
            const float NSeedX = Rng.FRandRange(0.0f, 500.0f);
            const float NSeedY = Rng.FRandRange(0.0f, 500.0f);
            const float NSeedZ = Rng.FRandRange(0.0f, 500.0f);
            const float PhaseA = Rng.FRandRange(0.0f, 2.0f * PI);
            // Base fBM frequency for the worm's wander (octaves add finer detail on top).
            const float BendFreq = Rng.FRandRange(1.5f, 2.5f);

            for (int32 s = 0; s <= Segments; s++)
            {
                const float T = (float)s / (float)Segments;
                float Z = FMath::Lerp(TopZ, BottomZ, T);
                const float Env = FMath::Sin(T * PI);  // 0 at both ends → mouths stay anchored
                float OX = 0.0f, OY = 0.0f;

                switch (Cfg.Style)
                {
                case EVoxelPassageStyle::Straight:
                    break;  // pure vertical

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

                Passage.ControlPoints.Add(FVector(PX + OX, PY + OY, Z));
                Passage.ControlRadii.Add(RadiusAt(T));
            }

            Passage.UpperPoint = Passage.ControlPoints[0];
            Passage.LowerPoint = Passage.ControlPoints.Last();
            Passage.Radius = FMath::Max(Cfg.MouthRadius, Cfg.MidRadius);  // fallback / bounds

            // Bounding sphere over all control points (+ widest radius + blend) for culling.
            {
                FVector Center = FVector::ZeroVector;
                for (const FVector& CP : Passage.ControlPoints) Center += CP;
                Center /= (float)Passage.ControlPoints.Num();
                float MaxDistSq = 0.0f;
                for (const FVector& CP : Passage.ControlPoints)
                    MaxDistSq = FMath::Max(MaxDistSq, (float)FVector::DistSquared(Center, CP));
                const float R = FMath::Sqrt(MaxDistSq) + Passage.Radius + 4.0f;
                Passage.BoundCenter = Center;
                Passage.BoundRadius = R;
                Passage.BoundRadiusSq = R * R;
            }

            Passages.Add(Passage);
        }
    }

    //=========================================================================
    // SURFACE ENTRY SHAFT — the one auto-opened (0,0) connection.
    // A straight vertical shaft at (0,0) piercing the TOP seal of the topmost
    // strate, so the world begins with "a hole opened to the surface". All other
    // (0,0) descents between strates remain player-dug.
    //=========================================================================
    if (bOpenSurfaceEntry && StrateLayout.Num() > 0)
    {
        const FStrateSlot& Top = StrateLayout[0];
        const float TopZ = (float)(Top.TopChunkZ + 1) * CHUNK_SIZE;

        FVoxelPassage Entry;
        Entry.UpperStrateIndex = 0;
        Entry.LowerStrateIndex = 0;
        Entry.PassageType = EVoxelPassageType::VerticalShaft;
        Entry.Radius = FMath::Max(OriginSpineRadius * 0.7f, 4.0f);
        // From a little above the strate top (open air outside all strates) down
        // past the seal into the interior, so the seal at (0,0) is breached.
        Entry.UpperPoint = FVector(0.0f, 0.0f, TopZ + CHUNK_SIZE);
        Entry.LowerPoint = FVector(0.0f, 0.0f, TopZ - CHUNK_SIZE);
        {
            const FVector C = (Entry.UpperPoint + Entry.LowerPoint) * 0.5f;
            const float R = (float)FVector::Dist(C, Entry.UpperPoint) + Entry.Radius + 4.0f;
            Entry.BoundCenter = C;
            Entry.BoundRadius = R;
            Entry.BoundRadiusSq = R * R;
        }
        Passages.Add(Entry);

        UE_LOG(LogTemp, Log, TEXT("[StrateManager] Surface entry shaft at (0,0) topZ=%.0f R=%.1f"),
            TopZ, Entry.Radius);
    }

    // Invalidate any thread_local per-chunk passage shortlists (see EvaluateModifierSDF).
    ++PassagesVersion;
}

//=============================================================================
// MODIFIER SDF (inter-strate passages)
//=============================================================================

float UVoxelStrateManager::EvaluateModifierSDF(float WorldX, float WorldY, float WorldZ) const
{
    //=========================================================================
    // PER-CHUNK PASSAGE SHORTLIST
    //=========================================================================
    // This runs PER VOXEL (35³ per tile). The vast majority of chunks are nowhere near a
    // descent passage, yet every voxel still walked the WHOLE Passages array just to reject
    // each one on a squared-distance test (Passages.Num() × 35³ rejects per tile, all wasted).
    // Cache, per chunk, the shortlist of passages whose bounds actually reach this chunk —
    // usually EMPTY → instant FLT_MAX return (no carve). Indices (not pointers) + a version
    // stamp keep it safe across a GeneratePassages rebuild. Output is bit-identical: the
    // shortlist is a conservative superset (chunk bounding sphere vs each passage bound).
    thread_local FIntVector    SL_Chunk(INT32_MAX, INT32_MAX, INT32_MAX);
    thread_local uint32        SL_Version = 0xFFFFFFFFu;
    thread_local TArray<int32> SL_Nearby;

    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));

    if (ChunkCoord != SL_Chunk || SL_Version != PassagesVersion)
    {
        SL_Chunk   = ChunkCoord;
        SL_Version = PassagesVersion;
        SL_Nearby.Reset();

        // Chunk bounding sphere (centre + half-diagonal), padded by the blend radius. A passage
        // is kept iff its bounding sphere overlaps the chunk's — i.e. some voxel here could be
        // inside its per-voxel reject radius. √3/2 · CHUNK_SIZE ≈ 0.866 · size.
        const FVector CCenter(
            (ChunkCoord.X + 0.5f) * (float)CHUNK_SIZE,
            (ChunkCoord.Y + 0.5f) * (float)CHUNK_SIZE,
            (ChunkCoord.Z + 0.5f) * (float)CHUNK_SIZE);
        const float ChunkR = (float)CHUNK_SIZE * 0.8660254f + 3.0f;  // +BlendK

        for (int32 i = 0; i < Passages.Num(); ++i)
        {
            const FVoxelPassage& P = Passages[i];
            const float Reach = P.BoundRadius + ChunkR;
            if (FVector::DistSquared(CCenter, P.BoundCenter) <= Reach * Reach)
            {
                SL_Nearby.Add(i);
            }
        }
    }

    if (SL_Nearby.Num() == 0) return FLT_MAX;   // no passage near this chunk → no carve

    float MinSDF = FLT_MAX;
    const float BlendK = 3.0f;  // Smooth blend for passage junctions

    //=========================================================================
    // PASSAGES — tapered capsule chains between strates (per-strate PassageConfig).
    // Each passage is a control-point chain with per-point radii; the (0,0) surface
    // entry is a simple straight tube. A bounding-sphere reject skips far passages.
    //=========================================================================
    const FVector Pos(WorldX, WorldY, WorldZ);
    for (int32 PIdx : SL_Nearby)
    {
        const FVoxelPassage& P = Passages[PIdx];
        // BOUNDING-SPHERE REJECT: skip passages this voxel can't possibly be inside.
        // EvaluateModifierSDF runs PER VOXEL and used to evaluate every passage's full
        // capsule chain unconditionally — the dominant lag source once passages became
        // 12-segment worms. Now far passages cost a single squared-distance compare.
        if (FVector::DistSquared(Pos, P.BoundCenter) > P.BoundRadiusSq) continue;

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
    }

    return MinSDF;
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

    return StrateLayout[SlotIdx].Definition->GeneratorType;
}

bool UVoxelStrateManager::UsesOperatorStackForChunk(const FIntVector& ChunkCoord) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition) { return false; }

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

    // Copy the designer-authored slab params from the strate definition.
    FSlabGenerationParams Result = Slot.Definition->SlabParams;

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
// No cross-boundary blending — archetypes meet at Hard boundaries. A macro keeps
// the boilerplate (slot lookup + fallback + Z bounds) in one place.

#define VF_ARCHETYPE_PARAMS_GETTER(FnName, StructType, DefMember)                       \
StructType UVoxelStrateManager::FnName(const FIntVector& ChunkCoord) const              \
{                                                                                      \
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);                              \
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)                              \
    {                                                                                  \
        StructType Empty;                                                              \
        Empty.BaseDensity = -1.0f;                                                     \
        return Empty;                                                                  \
    }                                                                                  \
    const FStrateSlot& Slot = StrateLayout[SlotIdx];                                   \
    StructType Result = Slot.Definition->DefMember;                                    \
    Result.StrateTopWorldZ    = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;              \
    Result.StrateBottomWorldZ = (float)(Slot.BottomChunkZ)  * CHUNK_SIZE;              \
    return Result;                                                                     \
}

VF_ARCHETYPE_PARAMS_GETTER(GetMazeParamsForChunk,           FMazeGenerationParams,    MazeParams)
VF_ARCHETYPE_PARAMS_GETTER(GetSurfaceParamsForChunk,        FSurfaceGenerationParams, SurfaceParams)
VF_ARCHETYPE_PARAMS_GETTER(GetVerticalShaftParamsForChunk,  FVerticalShaftParams,     VerticalShaftParams)
VF_ARCHETYPE_PARAMS_GETTER(GetFloatingIslandParamsForChunk, FFloatingIslandParams,    FloatingIslandParams)

#undef VF_ARCHETYPE_PARAMS_GETTER

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
    switch (Def->GeneratorType)
    {
    case ECaveGeneratorType::SurfaceWorld: Rel = Def->SurfaceParams.WaterLevelRelative;   break;
    case ECaveGeneratorType::Underwater:   Rel = Def->GenerationParams.WaterLevelRelative; break;
    default:                               Rel = Def->GenerationParams.WaterLevelRelative; break;
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
