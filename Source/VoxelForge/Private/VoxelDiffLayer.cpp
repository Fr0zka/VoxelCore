// VoxelDiffLayer.cpp
// Runtime terrain modification storage and evaluation.

#include "VoxelDiffLayer.h"

namespace
{
    std::atomic<uint64> GNextDiffLayerLifetimeId{0};

    bool IsFiniteVector(const FVector& Value)
    {
        return VoxelMath::IsFinite(Value.X)
            && VoxelMath::IsFinite(Value.Y)
            && VoxelMath::IsFinite(Value.Z);
    }

    bool IsValidModificationGeometry(const FVoxelModification& Mod)
    {
        if (!IsFiniteVector(Mod.Center) || !IsFiniteVector(Mod.BoxExtent)
            || !IsFiniteVector(Mod.CapsuleEnd)
            || !VoxelMath::IsFinite(Mod.Radius)
            || !VoxelMath::IsFinite(Mod.Strength)
            || !VoxelMath::IsFinite(Mod.Falloff)
            || Mod.Radius <= 0.0f || Mod.Falloff < 0.0f)
        {
            return false;
        }

        switch (Mod.Shape)
        {
        case EVoxelBrushShape::Sphere:
            return true;
        case EVoxelBrushShape::Box:
            return Mod.BoxExtent.X > 0.0f
                && Mod.BoxExtent.Y > 0.0f
                && Mod.BoxExtent.Z > 0.0f;
        case EVoxelBrushShape::Capsule:
            return true; // A coincident pair of endpoints is a valid spherical capsule.
        default:
            return false;
        }
    }

    double GetModificationBudgetVolume(const FVoxelModification& Mod)
    {
        switch (Mod.Shape)
        {
        case EVoxelBrushShape::Box:
        {
            // The SDF's falloff is a rounded outer band, so the expanded axis-aligned box is a
            // conservative charge and remains cheap to compute.
            const double Ex = static_cast<double>(Mod.BoxExtent.X) + static_cast<double>(Mod.Falloff);
            const double Ey = static_cast<double>(Mod.BoxExtent.Y) + static_cast<double>(Mod.Falloff);
            const double Ez = static_cast<double>(Mod.BoxExtent.Z) + static_cast<double>(Mod.Falloff);
            return 8.0 * Ex * Ey * Ez;
        }
        case EVoxelBrushShape::Capsule:
        {
            // Charge the full capsule including its falloff shell: cylinder + two hemispheres.
            const double OuterRadius = static_cast<double>(Mod.Radius) + static_cast<double>(Mod.Falloff);
            const double SegmentLength = static_cast<double>(FVector::Dist(Mod.Center, Mod.CapsuleEnd));
            return PI * OuterRadius * OuterRadius * SegmentLength
                + (4.0 / 3.0) * PI * OuterRadius * OuterRadius * OuterRadius;
        }
        case EVoxelBrushShape::Sphere:
        default:
        {
            const double Radius = static_cast<double>(Mod.Radius);
            return (4.0 / 3.0) * PI * Radius * Radius * Radius;
        }
        }
    }
}

uint64 UVoxelDiffLayer::GetCacheLifetimeId() const
{
    uint64 LifetimeId = CacheLifetimeId.load(std::memory_order_acquire);
    if (LifetimeId != 0)
    {
        return LifetimeId;
    }

    const uint64 NewLifetimeId = GNextDiffLayerLifetimeId.fetch_add(1, std::memory_order_relaxed) + 1;
    if (CacheLifetimeId.compare_exchange_strong(
        LifetimeId,
        NewLifetimeId,
        std::memory_order_release,
        std::memory_order_acquire))
    {
        return NewLifetimeId;
    }

    // Another worker initialized the identity first.  Its value is written into LifetimeId by
    // compare_exchange_strong on failure.
    return LifetimeId;
}

//=============================================================================
// BUDGET CONFIGURATION
//=============================================================================

void UVoxelDiffLayer::SetBudget(int32 InMaxMods, float InMaxRadius, float InMaxVolume)
{
    BudgetMaxMods = InMaxMods;
    BudgetMaxRadius = InMaxRadius;
    BudgetMaxVolume = InMaxVolume;

    UE_LOG(LogTemp, Log, TEXT("[DiffLayer] Budget set: MaxMods=%d, MaxRadius=%.1f, MaxVolume=%.0f (0=unlimited)"),
        BudgetMaxMods, BudgetMaxRadius, BudgetMaxVolume);
}

bool UVoxelDiffLayer::CanModify(float Radius) const
{
    if (!VoxelMath::IsFinite(Radius) || Radius <= 0.0f)
    {
        return false;
    }

    // Check radius limit (always enforced, even if "unlimited" count/volume)
    if (BudgetMaxRadius > 0.0f && Radius > BudgetMaxRadius)
    {
        return false;
    }

    // Check modification count limit
    if (BudgetMaxMods > 0 && ModificationCount >= BudgetMaxMods)
    {
        return false;
    }

    // Check volume limit: would this brush push us over?
    if (BudgetMaxVolume > 0.0f)
    {
        const double BrushVolume = (4.0 / 3.0) * PI
            * static_cast<double>(Radius) * static_cast<double>(Radius) * static_cast<double>(Radius);
        const double NewTotal = static_cast<double>(AccumulatedVolume) + BrushVolume;
        if (!VoxelMath::IsFinite(BrushVolume) || BrushVolume > static_cast<double>(FLT_MAX)
            || !VoxelMath::IsFinite(NewTotal)
            || NewTotal > static_cast<double>(BudgetMaxVolume))
        {
            return false;
        }
    }

    return true;
}

bool UVoxelDiffLayer::CanModify(const FVoxelModification& Mod) const
{
    if (!IsValidModificationGeometry(Mod)) return false;

    // Radius is the radial cap for spheres/capsules. A box's largest half-extent is its analogous
    // brush radius; checking it separately prevents a direct API caller from hiding a huge box behind
    // an unrelated small Radius field.
    const float GeometryRadius = (Mod.Shape == EVoxelBrushShape::Box)
        ? FMath::Max3(Mod.BoxExtent.X, Mod.BoxExtent.Y, Mod.BoxExtent.Z)
        : Mod.Radius;
    if (BudgetMaxRadius > 0.0f && GeometryRadius > BudgetMaxRadius) return false;
    if (BudgetMaxMods > 0 && ModificationCount >= BudgetMaxMods) return false;

    const double BrushVolume = GetModificationBudgetVolume(Mod);
    const double NewTotal = static_cast<double>(AccumulatedVolume) + BrushVolume;
    if (!VoxelMath::IsFinite(BrushVolume) || BrushVolume > static_cast<double>(FLT_MAX)
        || !VoxelMath::IsFinite(NewTotal)) return false;
    if (BudgetMaxVolume > 0.0f && NewTotal > static_cast<double>(BudgetMaxVolume)) return false;
    return true;
}

int32 UVoxelDiffLayer::GetRemainingModifications() const
{
    if (BudgetMaxMods <= 0) return -1;  // Unlimited
    return FMath::Max(0, BudgetMaxMods - ModificationCount);
}

float UVoxelDiffLayer::GetRemainingVolume() const
{
    if (BudgetMaxVolume <= 0.0f) return -1.0f;  // Unlimited
    return FMath::Max(0.0f, BudgetMaxVolume - AccumulatedVolume);
}

//=============================================================================
// APPLY MODIFICATION
//=============================================================================

TArray<FIntVector> UVoxelDiffLayer::ApplyModification(const FVoxelModification& Mod)
{
    TArray<FIntVector> AffectedChunks;

    // Validate the complete geometry before radius clamping, budget accounting, or AABB math.
    // FMath::Min(NaN, limit) and the shape switch's default sphere path would otherwise turn
    // malformed API input into either a counter update or an invalid chunk range.
    if (!IsValidModificationGeometry(Mod))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[DiffLayer] Modification REJECTED — non-finite or invalid brush geometry"));
        return AffectedChunks;
    }

    // --- Budget enforcement ---
    // Preserve the historical radius clamp for spheres/capsules when a finite per-brush cap is set;
    // the shape-aware check below still charges the resulting geometry and rejects oversized boxes.
    const float ClampedRadius = (BudgetMaxRadius > 0.0f)
        ? FMath::Min(Mod.Radius, BudgetMaxRadius) : Mod.Radius;
    FVoxelModification ClampedMod = Mod;
    ClampedMod.Radius = ClampedRadius;

    if (!CanModify(ClampedMod))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[DiffLayer] Modification REJECTED — budget exceeded (count=%d/%d, volume=%.0f/%.0f)"),
            ModificationCount, BudgetMaxMods, AccumulatedVolume, BudgetMaxVolume);
        return AffectedChunks;  // Empty = rejected
    }

    // Complete the world-AABB calculation before mutating the budget counters.  Finite input can
    // still overflow during Center +/- extent or address a chunk outside FIntVector's domain;
    // neither case is a modification that this layer can store safely.
    FVector BoundsMin, BoundsMax;
    ClampedMod.GetWorldBounds(BoundsMin, BoundsMax);
    auto WorldToChunk = [](float WorldCoord, int32& OutChunk) -> bool
    {
        if (!VoxelMath::IsFinite(WorldCoord))
        {
            return false;
        }
        const double ChunkCoord = FMath::FloorToDouble(
            static_cast<double>(WorldCoord) / static_cast<double>(CHUNK_SIZE));
        if (!VoxelMath::IsFinite(ChunkCoord)
            || ChunkCoord < static_cast<double>(MIN_int32)
            || ChunkCoord > static_cast<double>(MAX_int32))
        {
            return false;
        }
        OutChunk = static_cast<int32>(ChunkCoord);
        return true;
    };

    int32 MinCX = 0, MaxCX = 0, MinCY = 0, MaxCY = 0, MinCZ = 0, MaxCZ = 0;
    if (!WorldToChunk(BoundsMin.X, MinCX) || !WorldToChunk(BoundsMax.X, MaxCX)
        || !WorldToChunk(BoundsMin.Y, MinCY) || !WorldToChunk(BoundsMax.Y, MaxCY)
        || !WorldToChunk(BoundsMin.Z, MinCZ) || !WorldToChunk(BoundsMax.Z, MaxCZ))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[DiffLayer] Modification REJECTED — brush bounds are not representable"));
        return TArray<FIntVector>();
    }

    // Update budget counters
    ModificationCount++;
    const double BrushVolume = GetModificationBudgetVolume(ClampedMod);
    AccumulatedVolume += static_cast<float>(BrushVolume);

    {
        // Write lock: blocks worker-thread readers (HasModifications / GetDensityOffset) while the map
        // is mutated/rehashed. AffectedChunks is local; populate it in the same pass.
        FWriteScopeLock Lock(ModsLock);
        for (int32 CZ = MinCZ;; ++CZ)
        {
            for (int32 CY = MinCY;; ++CY)
            {
                for (int32 CX = MinCX;; ++CX)
                {
                    FIntVector ChunkCoord(CX, CY, CZ);

                    // Store the modification in this chunk's list
                    ChunkMods.FindOrAdd(ChunkCoord).Add(ClampedMod);

                    // Track which chunks need re-meshing
                    AffectedChunks.Add(ChunkCoord);

                    if (CX == MaxCX) break;
                }
                if (CY == MaxCY) break;
            }
            if (CZ == MaxCZ) break;
        }
        // Publish: subsequent readers must now take the lock instead of fast-rejecting, and
        // worker-side snapshots keyed on the version re-copy their chunk's list.
        bHasAnyMods.store(true, std::memory_order_release);
        ModsVersion.fetch_add(1, std::memory_order_release);
    }

    UE_LOG(LogTemp, Log,
        TEXT("[DiffLayer] Modification #%d at (%.0f, %.0f, %.0f) R=%.1f S=%.1f -> %d chunks (budget: %d/%d mods, %.0f/%.0f vol)"),
        ModificationCount,
        ClampedMod.Center.X, ClampedMod.Center.Y, ClampedMod.Center.Z,
        ClampedMod.Radius, ClampedMod.Strength,
        AffectedChunks.Num(),
        ModificationCount, BudgetMaxMods,
        AccumulatedVolume, BudgetMaxVolume);

    return AffectedChunks;
}

//=============================================================================
// DENSITY EVALUATION
//=============================================================================

float UVoxelDiffLayer::GetDensityOffset(const FIntVector& ChunkCoord,
                                         float WorldX, float WorldY, float WorldZ) const
{
    // Lock-free fast reject: no carves anywhere -> nothing to offset.
    if (!bHasAnyMods.load(std::memory_order_acquire)) return 0.0f;

    // Hold the read lock for the whole body: Mods points INTO the map and is dereferenced through the
    // falloff loop, so the map must not be rehashed by a concurrent writer meanwhile. NOTE: hot-path
    // callers (the generator) should prefer GetChunkModsSnapshot + EvaluateMods — one lock per chunk
    // instead of one per voxel.
    FReadScopeLock Lock(ModsLock);

    const TArray<FVoxelModification>* Mods = ChunkMods.Find(ChunkCoord);
    if (!Mods || Mods->Num() == 0) return 0.0f;

    return EvaluateMods(*Mods, WorldX, WorldY, WorldZ);
}

void UVoxelDiffLayer::GetChunkModsSnapshot(const FIntVector& ChunkCoord, TArray<FVoxelModification>& Out) const
{
    Out.Reset();
    if (!bHasAnyMods.load(std::memory_order_acquire)) return;

    FReadScopeLock Lock(ModsLock);
    if (const TArray<FVoxelModification>* Mods = ChunkMods.Find(ChunkCoord))
    {
        Out = *Mods;
    }
}

bool UVoxelDiffLayer::HasAnyModInChunkRange(const FIntVector& MinChunk, const FIntVector& MaxChunk) const
{
    if (!bHasAnyMods.load(std::memory_order_acquire)) return false;

    if (MinChunk.X > MaxChunk.X || MinChunk.Y > MaxChunk.Y || MinChunk.Z > MaxChunk.Z)
    {
        return false;
    }

    // ApplyModification registers a mod in every chunk its AABB overlaps, so looking up the keys in
    // the queried range is conservative and proportional to that range. Do not walk ChunkMods:
    // the edit history can grow without bound while a sealed-solid candidate is being proved.
    FReadScopeLock Lock(ModsLock);
    for (int32 CZ = MinChunk.Z;; ++CZ)
    {
        for (int32 CY = MinChunk.Y;; ++CY)
        {
            for (int32 CX = MinChunk.X;; ++CX)
            {
                if (const TArray<FVoxelModification>* Mods = ChunkMods.Find(FIntVector(CX, CY, CZ)))
                {
                    if (Mods->Num() > 0) return true;
                }
                if (CX == MaxChunk.X) break;
            }
            if (CY == MaxChunk.Y) break;
        }
        if (CZ == MaxChunk.Z) break;
    }
    return false;
}

float UVoxelDiffLayer::EvaluateMods(const TArray<FVoxelModification>& Mods,
                                    float WorldX, float WorldY, float WorldZ)
{
    float TotalOffset = 0.0f;
    const FVector Pos(WorldX, WorldY, WorldZ);

    // smoothstep helper (full strength when t<=0, zero when t>=1).
    auto Smooth01 = [](float T) -> float
    {
        T = FMath::Clamp(T, 0.0f, 1.0f);
        return 1.0f - T * T * (3.0f - 2.0f * T);
    };

    for (const FVoxelModification& Mod : Mods)
    {
        float Falloff = 0.0f;

        switch (Mod.Shape)
        {
        case EVoxelBrushShape::Box:
        {
            // Box SDF (negative inside), then fade over the Falloff band outside.
            const FVector Q = (Pos - Mod.Center).GetAbs() - Mod.BoxExtent;
            const float Outside = FVector(FMath::Max(Q.X, 0.0f), FMath::Max(Q.Y, 0.0f), FMath::Max(Q.Z, 0.0f)).Size();
            const float Inside = FMath::Min(FMath::Max3(Q.X, Q.Y, Q.Z), 0.0f);
            const float S = Outside + Inside;            // <=0 inside the box
            const float Band = FMath::Max(Mod.Falloff, 0.01f);
            if (S >= Band) continue;
            Falloff = Smooth01(FMath::Max(S, 0.0f) / Band);
            break;
        }
        case EVoxelBrushShape::Capsule:
        {
            // Distance to the segment minus the tube radius, faded over Falloff.
            const FVector AB = Mod.CapsuleEnd - Mod.Center;
            const FVector AP = Pos - Mod.Center;
            const float T = FMath::Clamp(FVector::DotProduct(AP, AB) / FMath::Max(FVector::DotProduct(AB, AB), KINDA_SMALL_NUMBER), 0.0f, 1.0f);
            const float SegDist = FVector::Dist(Pos, Mod.Center + AB * T);
            const float S = SegDist - Mod.Radius;        // <=0 inside the tube
            const float Band = FMath::Max(Mod.Falloff, 0.01f);
            if (S >= Band) continue;
            Falloff = Smooth01(FMath::Max(S, 0.0f) / Band);
            break;
        }
        case EVoxelBrushShape::Sphere:
        default:
        {
            const float Dist = FVector::Dist(Pos, Mod.Center);
            if (Dist >= Mod.Radius) continue;
            Falloff = Smooth01(Dist / Mod.Radius);
            break;
        }
        }

        TotalOffset += Mod.Strength * Falloff;
    }

    return TotalOffset;
}

bool UVoxelDiffLayer::HasModifications(const FIntVector& ChunkCoord) const
{
    // Lock-free fast reject: no carves anywhere -> the map is empty, skip lock + lookup entirely.
    if (!bHasAnyMods.load(std::memory_order_acquire)) return false;

    FReadScopeLock Lock(ModsLock);   // worker threads read concurrently; serialised vs. writers
    const TArray<FVoxelModification>* Mods = ChunkMods.Find(ChunkCoord);
    return Mods && Mods->Num() > 0;
}

//=============================================================================
// MANAGEMENT
//=============================================================================

void UVoxelDiffLayer::Clear()
{
    int32 Count = GetTotalModificationCount();
    {
        // Write lock: workers may be mid-read. Flip the fast-path flag false BEFORE emptying so any
        // reader that loads it afterwards skips the map without locking.
        FWriteScopeLock Lock(ModsLock);
        bHasAnyMods.store(false, std::memory_order_release);
        ChunkMods.Empty();
        ModsVersion.fetch_add(1, std::memory_order_release);   // invalidate worker snapshots
    }

    // Reset budget counters — player gets a fresh budget after clear/season reset
    ModificationCount = 0;
    AccumulatedVolume = 0.0f;

    UE_LOG(LogTemp, Log, TEXT("[DiffLayer] Cleared %d modifications, budget reset"), Count);
}

int32 UVoxelDiffLayer::GetTotalModificationCount() const
{
    FReadScopeLock Lock(ModsLock);
    int32 Total = 0;
    for (const auto& Pair : ChunkMods)
    {
        Total += Pair.Value.Num();
    }
    return Total;
}

int32 UVoxelDiffLayer::GetModifiedChunkCount() const
{
    FReadScopeLock Lock(ModsLock);
    return ChunkMods.Num();
}
