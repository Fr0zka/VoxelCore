// VoxelAtmosphereManager.cpp

#include "VoxelAtmosphereManager.h"
#include "VoxelStrateManager.h"
#include "VoxelStrateDefinition.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyLightComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

void UVoxelAtmosphereManager::Initialize(AActor* InOwner, UVoxelStrateManager* InStrateManager)
{
    Owner = InOwner;
    StrateManager = InStrateManager;

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

    // React only when the player changes strate (cheap on every other frame).
    if (Idx != CurrentStrateIndex)
    {
        CurrentStrateIndex = Idx;
        ApplyStrate(Def);
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

void UVoxelAtmosphereManager::ApplyStrate(const UVoxelStrateDefinition* Def)
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

    // ── Managed fog (only when NOT overridden) ──
    if (Fog)
    {
        if (!bUseOverride && Def && Def->FogDensity > 0.0f)
        {
            Fog->SetVisibility(true);
            // FogDensity is authored 0..1; EHF density is tiny — scale into a sane range.
            Fog->SetFogDensity(Def->FogDensity * 0.05f);
            Fog->SetFogInscatteringColor(Def->FogColor);
            Fog->SetVolumetricFog(Def->bVolumetricFog);
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
            Sky->SetIntensity(Def->AmbientLightIntensity);
            Sky->SetLightColor(Def->AmbientLightColor);
            Sky->SetLowerHemisphereColor(Def->AmbientLightColor);
            Sky->RecaptureSky();
        }
        else
        {
            Sky->SetIntensity(0.0f);
        }
    }

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
    if (Fog) Fog->SetVisibility(false);
}
