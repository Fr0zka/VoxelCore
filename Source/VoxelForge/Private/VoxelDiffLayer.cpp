// VoxelDiffLayer.cpp
// Runtime terrain modification storage and evaluation.

#include "VoxelDiffLayer.h"

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
    // Check radius limit (always enforced, even if "unlimited" count/volume)
    if (Radius > BudgetMaxRadius)
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
        const float BrushVolume = (4.0f / 3.0f) * PI * Radius * Radius * Radius;
        if (AccumulatedVolume + BrushVolume > BudgetMaxVolume)
        {
            return false;
        }
    }

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

    // --- Budget enforcement ---
    // Clamp radius to max allowed
    float ClampedRadius = FMath::Min(Mod.Radius, BudgetMaxRadius);

    if (!CanModify(ClampedRadius))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[DiffLayer] Modification REJECTED — budget exceeded (count=%d/%d, volume=%.0f/%.0f)"),
            ModificationCount, BudgetMaxMods, AccumulatedVolume, BudgetMaxVolume);
        return AffectedChunks;  // Empty = rejected
    }

    // Use clamped radius for the actual modification
    FVoxelModification ClampedMod = Mod;
    ClampedMod.Radius = ClampedRadius;

    // Update budget counters
    ModificationCount++;
    const float BrushVolume = (4.0f / 3.0f) * PI * ClampedRadius * ClampedRadius * ClampedRadius;
    AccumulatedVolume += BrushVolume;

    // Find every chunk the brush can touch, from its shape-aware world AABB.
    FVector BoundsMin, BoundsMax;
    ClampedMod.GetWorldBounds(BoundsMin, BoundsMax);
    const int32 MinCX = FMath::FloorToInt(BoundsMin.X / CHUNK_SIZE);
    const int32 MaxCX = FMath::FloorToInt(BoundsMax.X / CHUNK_SIZE);
    const int32 MinCY = FMath::FloorToInt(BoundsMin.Y / CHUNK_SIZE);
    const int32 MaxCY = FMath::FloorToInt(BoundsMax.Y / CHUNK_SIZE);
    const int32 MinCZ = FMath::FloorToInt(BoundsMin.Z / CHUNK_SIZE);
    const int32 MaxCZ = FMath::FloorToInt(BoundsMax.Z / CHUNK_SIZE);

    for (int32 CZ = MinCZ; CZ <= MaxCZ; CZ++)
    {
        for (int32 CY = MinCY; CY <= MaxCY; CY++)
        {
            for (int32 CX = MinCX; CX <= MaxCX; CX++)
            {
                FIntVector ChunkCoord(CX, CY, CZ);

                // Store the modification in this chunk's list
                ChunkMods.FindOrAdd(ChunkCoord).Add(ClampedMod);

                // Track which chunks need re-meshing
                AffectedChunks.Add(ChunkCoord);
            }
        }
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
    // Fast path: if this chunk has no modifications, return 0
    const TArray<FVoxelModification>* Mods = ChunkMods.Find(ChunkCoord);
    if (!Mods || Mods->Num() == 0) return 0.0f;

    float TotalOffset = 0.0f;
    const FVector Pos(WorldX, WorldY, WorldZ);

    // smoothstep helper (full strength when t<=0, zero when t>=1).
    auto Smooth01 = [](float T) -> float
    {
        T = FMath::Clamp(T, 0.0f, 1.0f);
        return 1.0f - T * T * (3.0f - 2.0f * T);
    };

    for (const FVoxelModification& Mod : *Mods)
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
    const TArray<FVoxelModification>* Mods = ChunkMods.Find(ChunkCoord);
    return Mods && Mods->Num() > 0;
}

//=============================================================================
// MANAGEMENT
//=============================================================================

void UVoxelDiffLayer::Clear()
{
    int32 Count = GetTotalModificationCount();
    ChunkMods.Empty();

    // Reset budget counters — player gets a fresh budget after clear/season reset
    ModificationCount = 0;
    AccumulatedVolume = 0.0f;

    UE_LOG(LogTemp, Log, TEXT("[DiffLayer] Cleared %d modifications, budget reset"), Count);
}

int32 UVoxelDiffLayer::GetTotalModificationCount() const
{
    int32 Total = 0;
    for (const auto& Pair : ChunkMods)
    {
        Total += Pair.Value.Num();
    }
    return Total;
}

int32 UVoxelDiffLayer::GetModifiedChunkCount() const
{
    return ChunkMods.Num();
}
