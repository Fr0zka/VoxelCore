// VoxelDensityPrimitives.h
// Les trois post-traitements STRUCTURELS partagés par chaque archétype.
// The three STRUCTURAL post-processes every archetype shares.
//
// POURQUOI CE FICHIER EXISTE / WHY THIS FILE EXISTS
// Ces trois fonctions étaient `static` dans VoxelGenerator.cpp et appelées à l'identique par les six
// fonctions de densité. La pile d'opérateurs a besoin des MÊMES, donc elles montent ici : UNE copie,
// partagée par le générateur et par les opérateurs. Dupliquer serait garantir qu'elles divergent —
// et ce sont des INVARIANTS de monde (la descente doit rester possible, les seals doivent tenir, les
// passages doivent percer), pas des choix créatifs.
//
// They were `static` in VoxelGenerator.cpp and called identically by all six density functions. The
// operator stack needs the same ones, so they move here: ONE copy, shared. Duplicating would
// guarantee divergence, and these are world INVARIANTS, not creative choices.
//
// ⚠️ CONVENTION DE SIGNE — la source n°1 de confusion du plugin.
// Ces trois fonctions travaillent en convention INTERNE : **positif = SOLIDE, négatif = AIR**.
// C'est la convention dans laquelle chaque fonction d'archétype est écrite ; la négation vers la
// convention marching-cubes (négatif = solide) se fait UNE FOIS, sur le `return`.
// SIGN CONVENTION: these work in INTERNAL convention — **positive = SOLID**. The negate to MC
// convention happens ONCE, at the caller's return.
//
// Aucun changement de comportement en les déplaçant : corps identiques, FORCEINLINE au lieu de
// static, mêmes appelants. / No behavioural change: identical bodies, FORCEINLINE instead of static.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTypes.h"   // SmoothStep01

//=============================================================================
// SEAL DE FRONTIÈRE / BOUNDARY SEAL
//=============================================================================
// Seal solide aux bords haut et bas de la strate. Fade smoothstep sur `Thickness` voxels depuis
// chaque bord. N'AJOUTE que de la densité (FMath::Max), jamais n'en enlève → le joueur ne peut
// jamais percer le seal "par accident", seulement via les passages.
//
// ⚠️ C'est un opérateur FORÇANT, pas seulement un FillOnly : à l'intérieur de la bande, avec
// SealFactor > 0 et BaseDensity > 0, le résultat est solide GARANTI quelle qu'ait été l'entrée.
// C'est exactement ce qu'encode `IVoxelDensityOp::ClassifyBox` (voir VoxelDensityOp.h), et la
// raison pour laquelle cette méthode existe.
FORCEINLINE void VF_ApplyBoundarySeal(float& Density, float WorldZ,
    float StrateTopZ, float StrateBottomZ,
    float Thickness, float BaseDensity)
{
    if (Thickness <= 0.0f) return;

    const float DistTop = StrateTopZ - WorldZ;     // + si on est sous le plafond
    const float DistBot = WorldZ - StrateBottomZ;  // + si on est au-dessus du sol

    if (DistTop >= 0.0f && DistTop < Thickness)
    {
        float SealFactor = 1.0f - (DistTop / Thickness);
        SealFactor = SmoothStep01(SealFactor);
        Density = FMath::Max(Density, SealFactor * BaseDensity);
    }
    if (DistBot >= 0.0f && DistBot < Thickness)
    {
        float SealFactor = 1.0f - (DistBot / Thickness);
        SealFactor = SmoothStep01(SealFactor);
        Density = FMath::Max(Density, SealFactor * BaseDensity);
    }
}

//=============================================================================
// CARVE DE PASSAGE / PASSAGE CARVING
//=============================================================================
// Creuse un passage inter-strates. Évalué APRÈS le seal pour que les passages puissent percer à
// travers le bouchon solide. Le rayon de blend hard-codé à 4.0f correspond à l'ancienne valeur —
// à exposer via UVoxelSettings si on veut pouvoir le tweaker.
FORCEINLINE void VF_ApplyPassageCarving(float& Density, float ModSDF,
    float BaseDensity, float SealThickness)
{
    constexpr float PASSAGE_BLEND_RADIUS = 4.0f;
    if (ModSDF >= PASSAGE_BLEND_RADIUS) return;

    float CarveFactor = FMath::Clamp(
        (PASSAGE_BLEND_RADIUS - ModSDF) / (PASSAGE_BLEND_RADIUS * 2.0f),
        0.0f, 1.0f);
    CarveFactor = SmoothStep01(CarveFactor);

    // FORCE the density toward guaranteed AIR so the passage punches through ANYTHING in
    // its path (seals, columns, surface roughness, terrain ops). A plain subtraction can
    // be out-paced by stacked density additions, leaving solid plugs mid-tunnel — which is
    // why the shaft "didn't go all the way through". Lerp toward a strongly negative target
    // and take the min so we only ever make it MORE air (never refill an existing cave).
    const float AirTarget = -(BaseDensity * 2.0f + SealThickness + 4.0f);
    Density = FMath::Min(Density, FMath::Lerp(Density, AirTarget, CarveFactor));
}

//=============================================================================
// SPINE DE DESCENTE (0,0) / (0,0) DESCENT SPINE
//=============================================================================
// Creuse une colonne verticale garantie ouverte au XY monde (0,0) dans l'INTÉRIEUR de la strate
// (entre les seals haut et bas). Les seals sont laissés intacts pour que le joueur doive encore
// creuser à travers pour descendre — ceci ne fait qu'un espace d'atterrissage propre, indépendant
// de l'archétype, aligné à travers toutes les strates.
FORCEINLINE void VF_ApplyOriginSpine(float& Density, float WorldX, float WorldY, float WorldZ,
    float StrateTopZ, float StrateBottomZ, float SealThickness, float BaseDensity, float Radius)
{
    if (Radius <= 0.0f) return;

    // Stay within the interior — never touch the seal bands.
    const float InnerTop = StrateTopZ - SealThickness;
    const float InnerBot = StrateBottomZ + SealThickness;
    if (WorldZ <= InnerBot || WorldZ >= InnerTop) return;

    const float DistXY = FMath::Sqrt(WorldX * WorldX + WorldY * WorldY);
    const float SDF = DistXY - Radius;  // < 0 inside the column
    const float Blend = 3.0f;
    if (SDF < Blend)
    {
        float Carve = FMath::Clamp((Blend - SDF) / (Blend * 2.0f), 0.0f, 1.0f);
        Carve = SmoothStep01(Carve);
        Density -= Carve * (BaseDensity * 2.0f + SealThickness);
    }
}

//=============================================================================
// PORTÉES / REACHES — les rayons dont ClassifyTile et EffectOverBox ont besoin
//=============================================================================
// Les constantes de blend ci-dessus (3.0 pour la spine, 4.0 pour les passages) sont dupliquées à la
// main dans ClassifyTile aujourd'hui. Les nommer ici pour qu'un futur test de bornes ne puisse pas
// les désynchroniser. / The blend constants above are hand-duplicated inside ClassifyTile today.
// Naming them here so a future bounds test cannot let the two drift apart.
namespace VoxelDensityReach
{
    constexpr float SpineBlend   = 3.0f;
    constexpr float PassageBlend = 4.0f;
}
