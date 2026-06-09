// VoxelContentManager.cpp
// Deterministic per-chunk decoration scatter + aesthetic water surfaces.

#include "VoxelContentManager.h"
#include "VoxelStrateManager.h"
#include "VoxelStrateDefinition.h"
#include "VoxelCaveMorphology.h"   // VoxelHash
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"

// Global safety cap on spawned decorations per chunk (across all entries).
static constexpr int32 GMaxDecorationsPerChunk = 400;

void UVoxelContentManager::Initialize(AActor* InOwner, UVoxelStrateManager* InStrateManager, int32 InSeed)
{
    Owner = InOwner;
    StrateManager = InStrateManager;
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

    // Decorations only on near (full-detail) chunks — they're game-thread actor spawns
    // and pointless on distant low-poly chunks. Water is one cheap plane, place it at any LOD.
    if (LODLevel == 0)
    {
        TArray<TWeakObjectPtr<AActor>> Spawned;
        SpawnDecorations(ChunkCoord, MeshData, Def, Spawned);
        if (Spawned.Num() > 0)
        {
            SpawnedActors.Add(ChunkCoord, MoveTemp(Spawned));
        }
    }

    SpawnWater(ChunkCoord, Def);
}

void UVoxelContentManager::SpawnDecorations(const FIntVector& ChunkCoord, const FVoxelMeshData& MeshData,
                                            const UVoxelStrateDefinition* Def, TArray<TWeakObjectPtr<AActor>>& Out)
{
    if (Def->Decorations.Num() == 0) return;

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

    int32 TotalSpawned = 0;

    for (int32 DecoIdx = 0; DecoIdx < Def->Decorations.Num(); ++DecoIdx)
    {
        const FStrateDecoration& Deco = Def->Decorations[DecoIdx];
        if (!Deco.ActorClass) continue;
        if (Deco.SpawnDensity <= 0.0f) continue;

        int32 SpawnedThisEntry = 0;

        for (int32 i = 0; i < NumVerts; ++i)
        {
            if (TotalSpawned >= GMaxDecorationsPerChunk) return;
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

            FActorSpawnParameters SpawnParams;
            SpawnParams.Owner = OwnerActor;
            SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

            AActor* NewActor = World->SpawnActor<AActor>(
                Deco.ActorClass,
                FTransform(BaseQ, SpawnPos, FVector(Scale)),
                SpawnParams);

            if (NewActor)
            {
                Out.Add(NewActor);
                ++SpawnedThisEntry;
                ++TotalSpawned;
            }
        }
    }
}

void UVoxelContentManager::SpawnWater(const FIntVector& ChunkCoord, const UVoxelStrateDefinition* Def)
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

    if (Def->WaterMaterial)
    {
        Plane->SetMaterial(0, Def->WaterMaterial);
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
    // Any water planes without decorations.
    TArray<FIntVector> WaterCoords;
    WaterPlanes.GetKeys(WaterCoords);
    for (const FIntVector& C : WaterCoords)
    {
        ClearChunk(C);
    }
    SpawnedActors.Empty();
    WaterPlanes.Empty();
}
