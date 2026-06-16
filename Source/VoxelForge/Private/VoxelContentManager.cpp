// VoxelContentManager.cpp
// Deterministic per-chunk decoration scatter + aesthetic water surfaces.

#include "VoxelContentManager.h"
#include "VoxelStrateManager.h"
#include "VoxelStrateDefinition.h"
#include "VoxelBiomeDefinition.h"
#include "VoxelGenerator.h"
#include "VoxelCaveMorphology.h"   // VoxelHash
#include "Components/StaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"

// Global safety cap on spawned decoration ACTORS per chunk (across all entries).
// HISM instances are exempt — they're batched render data, capped per entry by MaxPerChunk.
static constexpr int32 GMaxDecorationsPerChunk = 400;

void UVoxelContentManager::Initialize(AActor* InOwner, UVoxelStrateManager* InStrateManager,
                                      UVoxelGenerator* InGenerator, int32 InSeed)
{
    Owner = InOwner;
    StrateManager = InStrateManager;
    Generator = InGenerator;
    Seed = InSeed;

    // Engine unit plane — reused (scaled) for every water surface.
    if (!PlaneMesh)
    {
        PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    }
}

// Deterministic placement hash: pure function of (chunk, surface index, entry, salt, seed).
static FORCEINLINE uint32 PlacementHash(const FIntVector& C, int32 SurfIdx, int32 EntryIdx, uint32 Seed, uint32 Salt)
{
    uint32 H = VoxelHash::Cell(C.X, C.Y, Seed ^ Salt);
    H ^= VoxelHash::Mix((uint32)(C.Z * 0x9E3779B1u));
    H ^= VoxelHash::Mix((uint32)SurfIdx * 2654435761u);
    H ^= VoxelHash::Mix((uint32)EntryIdx * 40503u + 1u);
    return VoxelHash::Mix(H);
}

void UVoxelContentManager::PopulateChunk(const FIntVector& ChunkCoord, const FVoxelMeshData& MeshData, int32 LODLevel)
{
    // Re-meshing a chunk calls this again — start clean.
    ClearChunk(ChunkCoord);

    if (!StrateManager) return;
    const UVoxelStrateDefinition* Def = StrateManager->GetStrateForChunk(ChunkCoord);
    if (!Def) return;

    // Dominant biome for this chunk (center XY) selects the content + water overrides.
    // Per-chunk granularity: a chunk straddling a biome border takes its centre's biome
    // for the whole decoration SET (fine — the set is a choice, not a per-vertex blend).
    const UVoxelBiomeDefinition* Biome = nullptr;
    if (Generator)
    {
        const float CenterVoxelX = ((float)ChunkCoord.X + 0.5f) * CHUNK_SIZE;
        const float CenterVoxelY = ((float)ChunkCoord.Y + 0.5f) * CHUNK_SIZE;
        Biome = Generator->GetDominantBiomeAt(CenterVoxelX, CenterVoxelY, ChunkCoord.Z);
    }

    // Decorations come from the biome when it provides any, else the strate's list.
    const TArray<FStrateDecoration>& Decos =
        (Biome && Biome->Decorations.Num() > 0) ? Biome->Decorations : Def->Decorations;

    // Per-entry LOD gate inside SpawnDecorations (FStrateDecoration::MaxLODLevel):
    // actor entries usually stay LOD0-only; instanced entries may persist to LOD1-2.
    // Water is one cheap plane, place it at any LOD.
    {
        TArray<TWeakObjectPtr<AActor>> Spawned;
        SpawnDecorations(ChunkCoord, MeshData, Decos, LODLevel, Spawned);
        if (Spawned.Num() > 0)
        {
            SpawnedActors.Add(ChunkCoord, MoveTemp(Spawned));
        }
    }

    // Water plane: biome material override (level stays strate-global, see SpawnWater).
    UMaterialInterface* WaterMat = (Biome && Biome->WaterMaterial) ? Biome->WaterMaterial : Def->WaterMaterial;
    SpawnWater(ChunkCoord, Def, WaterMat);
}

void UVoxelContentManager::SpawnDecorations(const FIntVector& ChunkCoord, const FVoxelMeshData& MeshData,
                                            const TArray<FStrateDecoration>& Decorations, int32 LODLevel,
                                            TArray<TWeakObjectPtr<AActor>>& Out)
{
    if (Decorations.Num() == 0) return;

    AActor* OwnerActor = Owner.Get();
    if (!OwnerActor) return;
    UWorld* World = OwnerActor->GetWorld();
    if (!World) return;

    const int32 NumVerts = MeshData.Vertices.Num();
    if (NumVerts == 0) return;

    const FTransform OwnerXf = OwnerActor->GetActorTransform();

    // Water world-Z for the water-relative placement rule (voxel coords → world units).
    const float WaterVoxelZ = StrateManager->GetWaterLevelWorldZForChunk(ChunkCoord);
    const bool bHasWater = (WaterVoxelZ != -FLT_MAX);
    const float WaterWorldZ = bHasWater ? WaterVoxelZ * VOXEL_SIZE : -FLT_MAX;

    // Strate light culling: lights only live in the player's strate. INT32_MIN = player
    // strate not known yet (first frames) — leave lights on, the first SetActiveStrate
    // sweep corrects them.
    const int32 ChunkStrate = GetChunkStrateIndex(ChunkCoord);
    const bool bLightsOn = (ActiveStrateIndex == INT32_MIN) || (ChunkStrate == ActiveStrateIndex);

    int32 TotalSpawned = 0;

    for (int32 DecoIdx = 0; DecoIdx < Decorations.Num(); ++DecoIdx)
    {
        const FStrateDecoration& Deco = Decorations[DecoIdx];
        const bool bInstanced = (Deco.InstancedMesh != nullptr);
        if (!bInstanced && !Deco.ActorClass) continue;
        if (Deco.SpawnDensity <= 0.0f) continue;
        if (LODLevel > Deco.MaxLODLevel) continue;  // entry not wanted at this LOD

        // Lazily created on the first placement of this entry (avoids empty components).
        UHierarchicalInstancedStaticMeshComponent* HISM = nullptr;

        int32 SpawnedThisEntry = 0;

        for (int32 i = 0; i < NumVerts; ++i)
        {
            if (!bInstanced && TotalSpawned >= GMaxDecorationsPerChunk) break;  // actor cap
            if (SpawnedThisEntry >= Deco.MaxPerChunk) break;

            const FVector NormalWorld = MeshData.Normals.IsValidIndex(i)
                ? OwnerXf.TransformVectorNoScale(MeshData.Normals[i]).GetSafeNormal()
                : FVector::UpVector;

            // Surface classification by normal Z.
            const bool bFloor   = NormalWorld.Z >  0.5f;
            const bool bCeiling = NormalWorld.Z < -0.5f;
            const bool bWall    = !bFloor && !bCeiling;
            bool bMatches = true;
            switch (Deco.SurfacePlacement)
            {
            case ESurfaceType::Floor:   bMatches = bFloor;   break;
            case ESurfaceType::Wall:    bMatches = bWall;    break;
            case ESurfaceType::Ceiling: bMatches = bCeiling; break;
            case ESurfaceType::Any:     bMatches = true;     break;
            default:                    bMatches = true;     break;
            }
            if (!bMatches) continue;

            // Deterministic spawn gate.
            const uint32 H = PlacementHash(ChunkCoord, i, DecoIdx, (uint32)Seed, 0xDEC0u);
            if (VoxelHash::ToFloat01(H) > Deco.SpawnDensity) continue;

            const FVector PosWorld = OwnerXf.TransformPosition(MeshData.Vertices[i]);

            // Water-relative rule.
            if (Deco.bRequireWaterRelative && bHasWater)
            {
                const bool bBelow = PosWorld.Z < WaterWorldZ;
                if (bBelow != Deco.bPlaceBelowWater) continue;
            }

            // Transform.
            const FVector SpawnPos = PosWorld + NormalWorld * Deco.SurfaceOffset;

            FQuat BaseQ = Deco.bAlignToSurface
                ? FRotationMatrix::MakeFromZ(NormalWorld).ToQuat()
                : FQuat::Identity;
            if (Deco.bRandomYaw)
            {
                const float Yaw = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x59415721u)) * 2.0f * PI;
                const FVector Axis = Deco.bAlignToSurface ? NormalWorld : FVector::UpVector;
                BaseQ = FQuat(Axis, Yaw) * BaseQ;
            }

            const float ScaleT = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x5CA1E000u));
            const float Scale = FMath::Lerp(Deco.MinScale, Deco.MaxScale, ScaleT);

            const FTransform SpawnXf(BaseQ, SpawnPos, FVector(Scale));

            if (bInstanced)
            {
                // HISM path — batched instances, no actor. Pure visual: no collision,
                // engine handles frustum/occlusion culling per cluster.
                if (!HISM)
                {
                    HISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(OwnerActor);
                    HISM->SetStaticMesh(Deco.InstancedMesh);
                    HISM->SetMobility(EComponentMobility::Movable);
                    HISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                    HISM->RegisterComponent();
                    HISM->AttachToComponent(OwnerActor->GetRootComponent(),
                        FAttachmentTransformRules::KeepRelativeTransform);
                    ChunkInstances.FindOrAdd(ChunkCoord).Add(HISM);
                }
                HISM->AddInstance(SpawnXf, /*bWorldSpace=*/true);
                ++SpawnedThisEntry;
            }
            else
            {
                FActorSpawnParameters SpawnParams;
                SpawnParams.Owner = OwnerActor;
                SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

                AActor* NewActor = World->SpawnActor<AActor>(Deco.ActorClass, SpawnXf, SpawnParams);

                if (NewActor)
                {
                    // Strate light culling — wrong strate ⇒ lights start hidden.
                    if (!bLightsOn)
                    {
                        SetActorLightsEnabled(NewActor, false);
                    }
                    Out.Add(NewActor);
                    ++SpawnedThisEntry;
                    ++TotalSpawned;
                }
            }
        }
    }
}

//=============================================================================
// STRATE LIGHT CULLING
//=============================================================================
// Seals + the bedrock gap make strates light-tight by construction, but a shadowless
// point light shines straight through rock (blocking IS shadowing), and a shadowed one
// pays full shadow cost to render black. Lights can therefore only legitimately matter
// in the player's own strate — toggle them analytically on strate change instead of
// asking the GPU to discover it per-pixel.

int32 UVoxelContentManager::GetChunkStrateIndex(const FIntVector& ChunkCoord) const
{
    if (!StrateManager) return INT32_MIN;
    // GetStrateIndex expects Unreal world units (cm) — it converts cm→chunkZ internally.
    // (Previously this passed voxel-Z, so the strate match for light culling was wrong.)
    const float CenterWorldZ = ((float)ChunkCoord.Z + 0.5f) * CHUNK_SIZE * VOXEL_SIZE;
    return StrateManager->GetStrateIndex(CenterWorldZ);
}

void UVoxelContentManager::SetActorLightsEnabled(AActor* Actor, bool bEnabled)
{
    TInlineComponentArray<ULightComponent*> Lights(Actor);
    for (ULightComponent* Light : Lights)
    {
        if (Light)
        {
            Light->SetVisibility(bEnabled);
        }
    }
}

void UVoxelContentManager::SetActiveStrate(int32 PlayerStrateIndex)
{
    if (PlayerStrateIndex == ActiveStrateIndex) return;  // every-Tick fast path
    ActiveStrateIndex = PlayerStrateIndex;

    // Strate change (rare): sweep all populated chunks and toggle their actors' lights.
    for (const auto& Pair : SpawnedActors)
    {
        const bool bEnable = (GetChunkStrateIndex(Pair.Key) == ActiveStrateIndex);
        for (const TWeakObjectPtr<AActor>& A : Pair.Value)
        {
            if (AActor* Act = A.Get())
            {
                SetActorLightsEnabled(Act, bEnable);
            }
        }
    }
}

void UVoxelContentManager::SpawnWater(const FIntVector& ChunkCoord, const UVoxelStrateDefinition* Def,
                                      UMaterialInterface* WaterMaterial)
{
    if (!Def->bHasWater || !PlaneMesh) return;

    AActor* OwnerActor = Owner.Get();
    if (!OwnerActor) return;

    const float WaterVoxelZ = StrateManager->GetWaterLevelWorldZForChunk(ChunkCoord);
    if (WaterVoxelZ == -FLT_MAX) return;

    // Only the chunk whose vertical span contains the water surface gets a plane.
    const float ChunkVoxelZMin = (float)(ChunkCoord.Z) * CHUNK_SIZE;
    const float ChunkVoxelZMax = ChunkVoxelZMin + CHUNK_SIZE;
    if (WaterVoxelZ < ChunkVoxelZMin || WaterVoxelZ > ChunkVoxelZMax) return;

    // World-space center of this chunk's XY footprint, at the water surface Z.
    const float CenterVoxelX = ((float)ChunkCoord.X + 0.5f) * CHUNK_SIZE;
    const float CenterVoxelY = ((float)ChunkCoord.Y + 0.5f) * CHUNK_SIZE;
    const FVector LocalCenter(CenterVoxelX * VOXEL_SIZE, CenterVoxelY * VOXEL_SIZE, WaterVoxelZ * VOXEL_SIZE);
    const FVector WorldCenter = OwnerActor->GetActorTransform().TransformPosition(LocalCenter);

    UStaticMeshComponent* Plane = NewObject<UStaticMeshComponent>(OwnerActor);
    Plane->SetStaticMesh(PlaneMesh);
    Plane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Plane->SetCastShadow(false);
    Plane->RegisterComponent();
    Plane->AttachToComponent(OwnerActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
    Plane->SetWorldLocation(WorldCenter);

    // Engine plane is 100 uu square → scale to one chunk's world footprint.
    const float ChunkWorld = (float)CHUNK_SIZE * VOXEL_SIZE;
    Plane->SetWorldScale3D(FVector(ChunkWorld / 100.0f, ChunkWorld / 100.0f, 1.0f));

    if (WaterMaterial)
    {
        Plane->SetMaterial(0, WaterMaterial);
    }

    WaterPlanes.Add(ChunkCoord, Plane);
}

void UVoxelContentManager::ClearChunk(const FIntVector& ChunkCoord)
{
    if (TArray<TWeakObjectPtr<AActor>>* Actors = SpawnedActors.Find(ChunkCoord))
    {
        for (const TWeakObjectPtr<AActor>& A : *Actors)
        {
            if (AActor* Act = A.Get())
            {
                Act->Destroy();
            }
        }
        SpawnedActors.Remove(ChunkCoord);
    }

    if (TArray<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>>* Comps = ChunkInstances.Find(ChunkCoord))
    {
        for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& C : *Comps)
        {
            if (UHierarchicalInstancedStaticMeshComponent* Comp = C.Get())
            {
                Comp->DestroyComponent();
            }
        }
        ChunkInstances.Remove(ChunkCoord);
    }

    if (UStaticMeshComponent** Plane = WaterPlanes.Find(ChunkCoord))
    {
        if (*Plane)
        {
            (*Plane)->DestroyComponent();
        }
        WaterPlanes.Remove(ChunkCoord);
    }
}

void UVoxelContentManager::ClearAll()
{
    TArray<FIntVector> Coords;
    SpawnedActors.GetKeys(Coords);
    for (const FIntVector& C : Coords)
    {
        ClearChunk(C);
    }
    // Chunks that only have instances or water (no actors).
    TArray<FIntVector> InstanceCoords;
    ChunkInstances.GetKeys(InstanceCoords);
    for (const FIntVector& C : InstanceCoords)
    {
        ClearChunk(C);
    }
    TArray<FIntVector> WaterCoords;
    WaterPlanes.GetKeys(WaterCoords);
    for (const FIntVector& C : WaterCoords)
    {
        ClearChunk(C);
    }
    SpawnedActors.Empty();
    ChunkInstances.Empty();
    WaterPlanes.Empty();
}
