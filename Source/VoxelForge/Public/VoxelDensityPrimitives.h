// VoxelDensityPrimitives.h
// Les quatre post-traitements STRUCTURELS partagés par chaque archétype.
// The four STRUCTURAL post-processes every archetype shares.
//
// POURQUOI CE FICHIER EXISTE / WHY THIS FILE EXISTS
// Les trois primitives verticales étaient `static` dans VoxelGenerator.cpp et appelées à l'identique
// par les six fonctions de densité. La pile d'opérateurs a besoin des MÊMES, donc elles montent ici :
// UNE copie partagée. Le scellement XY est ajouté dans ce même fichier pour que les deux chemins
// partagent aussi la nouvelle invariance — les invariants de monde ne sont pas des choix créatifs.
//
// The three vertical primitives were `static` in VoxelGenerator.cpp and called identically by all six
// density functions. The operator stack needs the same ones, so they move here: ONE shared copy. The
// XY seal is added beside them so both paths share the new invariant too.
//
// ⚠️ CONVENTION DE SIGNE — la source n°1 de confusion du plugin.
// Ces primitives travaillent en convention INTERNE : **positif = SOLIDE, négatif = AIR**.
// C'est la convention dans laquelle chaque fonction d'archétype est écrite ; la négation vers la
// convention marching-cubes (négatif = solide) se fait UNE FOIS, sur le `return`.
// SIGN CONVENTION: these work in INTERNAL convention — **positive = SOLID**. The negate to MC
// convention happens ONCE, at the caller's return.
//
// Les trois corps déplacés restent identiques; le quatrième est la nouvelle frontière globale.
// The three moved bodies remain identical; the fourth is the new global boundary.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTypes.h"   // SmoothStep01
#include "VoxelPassageGeometry.h" // shared body-sized landing/tunnel geometry

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

// Landing air is also reasserted after the MC-space disturbance post.  Unlike the legacy tube
// carve above, this threshold is independent of the incoming density: applying it a second time
// is therefore bit-stable.  The structural passage op and the post-disturbance backstop can share
// the same operation without turning an otherwise unchanged live-vs-direct op-stack comparison
// into a second, deeper smooth carve.  The tube intentionally retains its old current-density
// interpolation because that is part of the legacy structural path's contract.
FORCEINLINE void VF_ApplyPassageLandingCarving(float& Density, float LandingSDF,
    float BaseDensity, float SealThickness)
{
    constexpr float PASSAGE_BLEND_RADIUS = 4.0f;
    if (LandingSDF >= PASSAGE_BLEND_RADIUS) return;

    float CarveFactor = FMath::Clamp(
        (PASSAGE_BLEND_RADIUS - LandingSDF) / (PASSAGE_BLEND_RADIUS * 2.0f),
        0.0f, 1.0f);
    CarveFactor = SmoothStep01(CarveFactor);

    const float AirTarget = -(BaseDensity * 2.0f + SealThickness + 4.0f);
    const float LandingThreshold = FMath::Lerp(BaseDensity, AirTarget, CarveFactor);
    Density = FMath::Min(Density, LandingThreshold);
}

//=============================================================================
// LANDING ROOM ORIGINE (0,0) / ORIGIN LANDING ROOM
//=============================================================================
// The old implementation carved a full-height cylinder. That made the origin visually open but
// left no floor anywhere except at the bottom, so it was a fall shaft rather than a landing. The
// origin primitive is now a top-anchored rounded room with a real flat floor in every strate.
// It never crosses either vertical seal. The inter-strate seal remains solid until a passage (or
// the player) opens it; there is no default vertical bore between rooms.
FORCEINLINE void VF_ApplyOriginSpine(float& Density, float WorldX, float WorldY, float WorldZ,
    float StrateTopZ, float StrateBottomZ, float SealThickness, float BaseDensity, float Radius)
{
    if (Radius <= 0.0f) return;

    const VoxelPassageGeometry::FOriginLandingGeometry Geometry =
        VoxelPassageGeometry::BuildOriginLandingGeometry(
            StrateTopZ, StrateBottomZ, SealThickness, Radius);
    if (!Geometry.bValid) return;

    const float RoomSDF = VoxelPassageGeometry::OriginLandingRoomSDF(
        FVector(WorldX, WorldY, WorldZ), Geometry);
    VF_ApplyPassageLandingCarving(
        Density, RoomSDF, BaseDensity, SealThickness);

    // The floor is intentionally written after the room carve. It is also reasserted after the
    // passage/disturbance posts by the callers below; doing it here makes the standalone primitive
    // itself a complete landing-room operation.
    if (WorldZ <= Geometry.FloorZ + KINDA_SMALL_NUMBER
        && WorldZ > Geometry.FloorZ - Geometry.FloorThickness
        && FMath::Abs(WorldX) <= FMath::Max(Geometry.HalfWidth - 1.0f, 0.0f)
        && FMath::Abs(WorldY) <= FMath::Max(Geometry.HalfWidth - 1.0f, 0.0f))
    {
        if (!VoxelPassageGeometry::VerticalShaftConnectorAirMarker())
        {
            Density = FMath::Max(Density, BaseDensity);
        }
    }
}

/** Reassert only the origin-room air volume after a post-process that may add rock. */
FORCEINLINE void VF_ApplyOriginLandingAir(float& Density,
    float WorldX, float WorldY, float WorldZ,
    float StrateTopZ, float StrateBottomZ, float SealThickness, float BaseDensity, float Radius)
{
    if (Radius <= 0.0f) return;
    const VoxelPassageGeometry::FOriginLandingGeometry Geometry =
        VoxelPassageGeometry::BuildOriginLandingGeometry(
            StrateTopZ, StrateBottomZ, SealThickness, Radius);
    if (!Geometry.bValid) return;
    const float RoomSDF = VoxelPassageGeometry::OriginLandingRoomSDF(
        FVector(WorldX, WorldY, WorldZ), Geometry);
    VF_ApplyPassageLandingCarving(
        Density, RoomSDF, BaseDensity, SealThickness);
}

/** Reassert only the origin-room support slab after an air carve/post-process. */
FORCEINLINE void VF_ApplyOriginLandingFloor(float& Density,
    float WorldX, float WorldY, float WorldZ,
    float StrateTopZ, float StrateBottomZ, float SealThickness, float BaseDensity, float Radius)
{
    if (Radius <= 0.0f) return;
    const VoxelPassageGeometry::FOriginLandingGeometry Geometry =
        VoxelPassageGeometry::BuildOriginLandingGeometry(
            StrateTopZ, StrateBottomZ, SealThickness, Radius);
    if (!Geometry.bValid) return;
    if (WorldZ <= Geometry.FloorZ + KINDA_SMALL_NUMBER
        && WorldZ > Geometry.FloorZ - Geometry.FloorThickness
        && FMath::Abs(WorldX) <= FMath::Max(Geometry.HalfWidth - 1.0f, 0.0f)
        && FMath::Abs(WorldY) <= FMath::Max(Geometry.HalfWidth - 1.0f, 0.0f))
    {
        if (!VoxelPassageGeometry::VerticalShaftConnectorAirMarker())
        {
            Density = FMath::Max(Density, BaseDensity);
        }
    }
}

/** MC-facing origin-room air backstop for the post-disturbance path. */
FORCEINLINE void VF_ApplyOriginLandingAirMC(float& Density,
    float WorldX, float WorldY, float WorldZ,
    float StrateTopZ, float StrateBottomZ, float SealThickness, float BaseDensity, float Radius)
{
    float InternalDensity = -Density;
    VF_ApplyOriginLandingAir(InternalDensity, WorldX, WorldY, WorldZ,
        StrateTopZ, StrateBottomZ, SealThickness, BaseDensity, Radius);
    Density = -InternalDensity;
}

/** MC-facing origin-room floor backstop for the post-disturbance path. */
FORCEINLINE void VF_ApplyOriginLandingFloorMC(float& Density,
    float WorldX, float WorldY, float WorldZ,
    float StrateTopZ, float StrateBottomZ, float SealThickness, float BaseDensity, float Radius)
{
    float InternalDensity = -Density;
    VF_ApplyOriginLandingFloor(InternalDensity, WorldX, WorldY, WorldZ,
        StrateTopZ, StrateBottomZ, SealThickness, BaseDensity, Radius);
    Density = -InternalDensity;
}

//=============================================================================
// XY EDGE SEAL / SCELLEMENT DE LA LIMITE XY
//=============================================================================
// The bounded-world rim is a radial solid ramp in actor-space XY. This helper uses the same
// internal convention as VF_ApplyBoundarySeal (positive = solid); the MC-facing wrapper below
// negates it so the output convention remains negative = solid.
//
// Radius == 0 is deliberately checked before any arithmetic: it is a true legacy no-op, not a
// nearly-invisible setting-dependent perturbation. The common interior case also returns from a
// squared-distance test and pays no sqrt.
FORCEINLINE void VF_ApplyXYEdgeSeal(float& Density, float WorldX, float WorldY,
    float WorldRadiusVoxels, float Thickness, float BaseDensity)
{
    if (!(WorldRadiusVoxels > 0.0f) || !(Thickness > 0.0f) || !(BaseDensity > 0.0f)) return;
    if (!VoxelMath::IsFinite(WorldX) || !VoxelMath::IsFinite(WorldY)
        || !VoxelMath::IsFinite(WorldRadiusVoxels) || !VoxelMath::IsFinite(Thickness)
        || !VoxelMath::IsFinite(BaseDensity)) return;

    const float InnerRadius = WorldRadiusVoxels - Thickness;
    const float InnerRadiusSq = InnerRadius * InnerRadius;
    const float DistSq = WorldX * WorldX + WorldY * WorldY;
    const float RadiusSq = WorldRadiusVoxels * WorldRadiusVoxels;

    // The overwhelmingly common case: well inside the bounded world.
    if (InnerRadius > 0.0f && DistSq <= InnerRadiusSq) return;

    // Keep the outer-boundary comparison in squared space too. This makes the exact force branch
    // and the ClassifyBox proof use the same predicate: at or beyond R the result is BaseDensity,
    // with no sqrt-rounding ambiguity in the forced-solid margin.
    if (DistSq >= RadiusSq)
    {
        Density = FMath::Max(Density, BaseDensity);
        return;
    }

    const float DistXY = FMath::Sqrt(DistSq);
    // 0 at the inner edge, 1 at the world radius; smoothstep removes a cylindrical cliff.
    const float SealFactor = SmoothStep01(FMath::Clamp(
        (DistXY - InnerRadius) / Thickness, 0.0f, 1.0f));

    // At/beyond WorldRadiusVoxels the branch above already forced exactly BaseDensity.
    Density = FMath::Max(Density, SealFactor * BaseDensity);
}

/** MC-facing form used after post-processes that already operate in the output convention. */
FORCEINLINE void VF_ApplyXYEdgeSealMC(float& Density, float WorldX, float WorldY,
    float WorldRadiusVoxels, float Thickness, float BaseDensity)
{
    if (!(WorldRadiusVoxels > 0.0f) || !(Thickness > 0.0f)) return;

    float InternalDensity = -Density;
    VF_ApplyXYEdgeSeal(InternalDensity, WorldX, WorldY,
                       WorldRadiusVoxels, Thickness, BaseDensity);
    Density = -InternalDensity;
}

//=============================================================================
// XY EDGE PROOF / PREUVE DE BOITE
//=============================================================================
// The closest point of an AABB to the XY origin gives the minimum radius over the whole box.
// The helpers below are shared by ClassifyBox, ClassifyTile's global shell early-out, and the
// soundness test. They intentionally require one voxel of radial margin in the ramp: at the
// mathematical inner edge the helper is a true no-op, so claiming AllSolid there would be false.
namespace VoxelDensityReach
{
    constexpr float SpineBlend        = 3.0f;
    constexpr float PassageBlend      = 4.0f;
    constexpr float EdgeProofMargin   = 1.0f;
}

FORCEINLINE bool VF_IsValidXYEdgeSealProofInput(const FBox& VoxelBox,
    float WorldRadiusVoxels, float Thickness, float BaseDensity)
{
    return VoxelBox.IsValid
        && VoxelMath::IsFinite(VoxelBox.Min.X) && VoxelMath::IsFinite(VoxelBox.Min.Y)
        && VoxelMath::IsFinite(VoxelBox.Max.X) && VoxelMath::IsFinite(VoxelBox.Max.Y)
        && VoxelMath::IsFinite(WorldRadiusVoxels) && VoxelMath::IsFinite(Thickness)
        && VoxelMath::IsFinite(BaseDensity)
        && WorldRadiusVoxels > 0.0f && Thickness > 0.0f && BaseDensity > 0.0f;
}

FORCEINLINE float VF_XYEdgeClosestRadiusSq(const FBox& VoxelBox)
{
    const float ClosestX = (VoxelBox.Min.X > 0.0f) ? VoxelBox.Min.X
                         : (VoxelBox.Max.X < 0.0f) ? VoxelBox.Max.X : 0.0f;
    const float ClosestY = (VoxelBox.Min.Y > 0.0f) ? VoxelBox.Min.Y
                         : (VoxelBox.Max.Y < 0.0f) ? VoxelBox.Max.Y : 0.0f;
    return ClosestX * ClosestX + ClosestY * ClosestY;
}

FORCEINLINE float VF_XYEdgeFarthestRadiusSq(const FBox& VoxelBox)
{
    const float FarthestX = FMath::Max(FMath::Abs(VoxelBox.Min.X), FMath::Abs(VoxelBox.Max.X));
    const float FarthestY = FMath::Max(FMath::Abs(VoxelBox.Min.Y), FMath::Abs(VoxelBox.Max.Y));
    return FarthestX * FarthestX + FarthestY * FarthestY;
}

/**
 * Return the actual positive lower-bound margin imposed by the edge seal over a box, or 0 when
 * the box is not provably forced. The ramp bound is intentionally reduced by 2× after the
 * one-voxel proof margin, so float evaluation error can only make this estimate too small.
 */
FORCEINLINE float VF_XYEdgeSealForcedMarginOverBox(const FBox& VoxelBox,
    float WorldRadiusVoxels, float Thickness, float BaseDensity)
{
    if (!VF_IsValidXYEdgeSealProofInput(VoxelBox, WorldRadiusVoxels, Thickness, BaseDensity)) return 0.0f;

    const float ClosestRadiusSq = VF_XYEdgeClosestRadiusSq(VoxelBox);
    const float RadiusSq = WorldRadiusVoxels * WorldRadiusVoxels;
    if (ClosestRadiusSq >= RadiusSq)
    {
        // The per-voxel helper takes the >= WorldRadiusVoxels branch and forces BaseDensity.
        return BaseDensity;
    }

    const float InnerRadius = WorldRadiusVoxels - Thickness;
    const float ProofRadius = InnerRadius + VoxelDensityReach::EdgeProofMargin;
    if (!(Thickness > VoxelDensityReach::EdgeProofMargin)
        || !(ClosestRadiusSq >= ProofRadius * ProofRadius)) return 0.0f;

    const float GuaranteedFactor = SmoothStep01(
        FMath::Clamp(VoxelDensityReach::EdgeProofMargin / Thickness, 0.0f, 1.0f));
    return BaseDensity * GuaranteedFactor * 0.5f;
}

FORCEINLINE bool VF_XYEdgeSealBoxIsForcedSolid(const FBox& VoxelBox,
    float WorldRadiusVoxels, float Thickness, float BaseDensity)
{
    return VF_XYEdgeSealForcedMarginOverBox(
        VoxelBox, WorldRadiusVoxels, Thickness, BaseDensity) > 0.0f;
}

FORCEINLINE bool VF_XYEdgeSealBoxTouchesBand(const FBox& VoxelBox,
    float WorldRadiusVoxels, float Thickness)
{
    if (!(WorldRadiusVoxels > 0.0f) || !(Thickness > 0.0f)
        || !VoxelMath::IsFinite(WorldRadiusVoxels) || !VoxelMath::IsFinite(Thickness)) return false;

    const float InnerRadius = WorldRadiusVoxels - Thickness;
    return VF_XYEdgeFarthestRadiusSq(VoxelBox) > InnerRadius * InnerRadius;
}

//=============================================================================
// PORTÉES / REACHES — les rayons dont ClassifyTile et EffectOverBox ont besoin
//=============================================================================
// The constants are named above so a future bounds test cannot let them drift from the hot path.
