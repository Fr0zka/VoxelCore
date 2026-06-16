// VoxelAtmosphereManager.cpp

#include "VoxelAtmosphereManager.h"
#include "VoxelStrateManager.h"
#include "VoxelStrateDefinition.h"
#include "VoxelBiomeDefinition.h"
#include "VoxelGenerator.h"
#include "VoxelTypes.h"   // CHUNK_SIZE / VOXEL_SIZE
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyLightComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

void UVoxelAtmosphereManager::Initialize(AActor* InOwner, UVoxelStrateManager* InStrateManager, UVoxelGenerator* InGenerator)
{
    Owner = InOwner;
    StrateManager = InStrateManager;
    Generator = InGenerator;

    if (!InOwner) return;
    USceneComponent* Root = InOwner->GetRootComponent();

    // Managed height fog. Hidden until a strate with fog is entered.
    Fog = NewObject<UExponentialHeightFogComponent>(InOwner);
    Fog->SetMobility(EComponentMobility::Movable);
    Fog->RegisterComponent();
    if (Root) Fog->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
    Fog->SetVisibility(false);

    // Managed skylight for controllable ambient (movable so we can change it live).
    Sky = NewObject<USkyLightComponent>(InOwner);
    Sky->SetMobility(EComponentMobility::Movable);
    Sky->SourceType = ESkyLightSourceType::SLS_CapturedScene;
    Sky->bLowerHemisphereIsBlack = false;   // let LowerHemisphereColor act as flat ambient
    Sky->RegisterComponent();
    if (Root) Sky->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
}

void UVoxelAtmosphereManager::UpdateForPlayer(const FVector& PlayerWorldPos)
{
    if (!StrateManager) return;
    AActor* O = Owner.Get();
    if (!O) return;

    const int32 Idx = StrateManager->GetStrateIndex(PlayerWorldPos.Z);
    const UVoxelStrateDefinition* Def = StrateManager->GetStrateAt(PlayerWorldPos.Z);

    // Player's dominant biome (XY field) — atmosphere can vary by biome within a strate.
    // Convention matches GetStrateAt: world cm = voxel * VOXEL_SIZE (actor at origin/identity).
    const UVoxelBiomeDefinition* Biome = nullptr;
    if (Generator && Def)
    {
        const int32 ChunkZ = FMath::FloorToInt((PlayerWorldPos.Z / VOXEL_SIZE) / CHUNK_SIZE);
        Biome = Generator->GetDominantBiomeAt(PlayerWorldPos.X / VOXEL_SIZE,
                                              PlayerWorldPos.Y / VOXEL_SIZE, ChunkZ);
    }

    // React on strate change (full apply) OR biome change within a strate (fog/sky only —
    // layer actors + atmosphere BP are strate-level). Cheap no-op otherwise.
    if (Idx != CurrentStrateIndex)
    {
        CurrentStrateIndex = Idx;
        CurrentBiome = Biome;
        ApplyStrate(Def, Biome);
    }
    else if (Biome != CurrentBiome.Get())
    {
        CurrentBiome = Biome;
        ApplyFogSky(Def, Biome);
    }

    // Anchor the full-atmosphere BP to the player (so any localized volumes/effects
    // inside it follow). Global fog/skylight ignore position, so this is harmless.
    if (AtmosphereActorInstance)
    {
        AtmosphereActorInstance->SetActorLocation(PlayerWorldPos);
    }

    // Keep the layer actors centered on the player in XY, glued to the strate bounds,
    // with the authored rotation applied.
    if (Def)
    {
        float TopZ, BottomZ;
        if (StrateManager->GetStrateUnrealZRange(PlayerWorldPos.Z, TopZ, BottomZ))
        {
            if (CeilingActor)
            {
                CeilingActor->SetActorLocationAndRotation(
                    FVector(PlayerWorldPos.X, PlayerWorldPos.Y, TopZ + Def->CeilingLayerZOffset),
                    Def->CeilingLayerRotation);
            }
            if (FloorActor)
            {
                FloorActor->SetActorLocationAndRotation(
                    FVector(PlayerWorldPos.X, PlayerWorldPos.Y, BottomZ + Def->FloorLayerZOffset),
                    Def->FloorLayerRotation);
            }
        }
    }
}

void UVoxelAtmosphereManager::ApplyFogSky(const UVoxelStrateDefinition* Def, const UVoxelBiomeDefinition* Biome)
{
    // A strate's full atmosphere BP owns the entire look — managed fog/sky stay off.
    const bool bUseOverride = (Def && Def->AtmosphereActor);

    // Biome retint beats the strate's fog/sky when the biome opts in.
    const bool bBiome = (Biome && Biome->bOverrideAtmosphere);
    const FLinearColor FogCol = bBiome ? Biome->FogColor            : (Def ? Def->FogColor            : FLinearColor::Black);
    const float        FogDen = bBiome ? Biome->FogDensity          : (Def ? Def->FogDensity          : 0.0f);
    const FLinearColor AmbCol = bBiome ? Biome->AmbientLightColor   : (Def ? Def->AmbientLightColor   : FLinearColor::Black);
    const float        AmbInt = bBiome ? Biome->AmbientLightIntensity : (Def ? Def->AmbientLightIntensity : 0.0f);
    const bool         bVol   = Def ? Def->bVolumetricFog : false;

    // ── Managed fog (only when NOT overridden by a full atmosphere BP) ──
    if (Fog)
    {
        if (!bUseOverride && FogDen > 0.0f)
        {
            Fog->SetVisibility(true);
            // FogDensity is authored 0..1; EHF density is tiny — scale into a sane range.
            Fog->SetFogDensity(FogDen * 0.05f);
            Fog->SetFogInscatteringColor(FogCol);
            Fog->SetVolumetricFog(bVol);
        }
        else
        {
            Fog->SetVisibility(false);
        }
    }

    // ── Managed ambient skylight (only when NOT overridden) ──
    if (Sky)
    {
        if (!bUseOverride && Def)
        {
            Sky->SetIntensity(AmbInt);
            Sky->SetLightColor(AmbCol);
            Sky->SetLowerHemisphereColor(AmbCol);
            Sky->RecaptureSky();
        }
        else
        {
            Sky->SetIntensity(0.0f);
        }
    }
}

void UVoxelAtmosphereManager::ApplyStrate(const UVoxelStrateDefinition* Def, const UVoxelBiomeDefinition* Biome)
{
    AActor* O = Owner.Get();
    UWorld* W = O ? O->GetWorld() : nullptr;

    FActorSpawnParameters Params;
    Params.Owner = O;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    // ── FULL ATMOSPHERE OVERRIDE ──
    // If the strate provides its own atmosphere BP, it owns the entire look; spawn it
    // and disable the plugin's managed fog/skylight so they don't double up.
    if (AtmosphereActorInstance) { AtmosphereActorInstance->Destroy(); AtmosphereActorInstance = nullptr; }

    const bool bUseOverride = (Def && Def->AtmosphereActor);
    if (bUseOverride && W)
    {
        AtmosphereActorInstance = W->SpawnActor<AActor>(Def->AtmosphereActor, FTransform::Identity, Params);
    }

    // ── Managed fog + ambient skylight (biome-aware) ──
    ApplyFogSky(Def, Biome);

    // ── Ceiling / floor layer actors — destroy old, spawn new for this strate ──
    // (Independent of the override — you can have cloud seas with either fog path.)
    if (CeilingActor) { CeilingActor->Destroy(); CeilingActor = nullptr; }
    if (FloorActor)   { FloorActor->Destroy();   FloorActor   = nullptr; }

    if (Def && W)
    {
        if (Def->CeilingLayerActor)
        {
            CeilingActor = W->SpawnActor<AActor>(Def->CeilingLayerActor, FTransform::Identity, Params);
        }
        if (Def->FloorLayerActor)
        {
            FloorActor = W->SpawnActor<AActor>(Def->FloorLayerActor, FTransform::Identity, Params);
        }
    }
}

void UVoxelAtmosphereManager::Reset()
{
    if (AtmosphereActorInstance) { AtmosphereActorInstance->Destroy(); AtmosphereActorInstance = nullptr; }
    if (CeilingActor) { CeilingActor->Destroy(); CeilingActor = nullptr; }
    if (FloorActor)   { FloorActor->Destroy();   FloorActor   = nullptr; }
    CurrentStrateIndex = INT32_MIN;
    CurrentBiome = nullptr;
    if (Fog) Fog->SetVisibility(false);
}
