// Serialized strate-definition migrations.

#include "VoxelStrateDefinition.h"

#include "Serialization/CustomVersion.h"
#include "VoxelPassageGeometry.h"

namespace
{
    // The field name is retained for package compatibility, but version 1 changes its meaning
    // from a raw tangent/gradient to authored degrees. This GUID is local to the plugin.
    const FGuid GVoxelForgeStrateSerializationVersion(
        0xA76D5E31, 0x4B2C4F9A, 0xB8C1D204, 0x6E9F7738);
    constexpr int32 GVoxelForgeStrateGentleSlopeDegreesVersion = 1;
    FCustomVersionRegistration GVoxelForgeStrateSerializationRegistration(
        GVoxelForgeStrateSerializationVersion,
        GVoxelForgeStrateGentleSlopeDegreesVersion,
        TEXT("VoxelForgeStrateSerialization"));

    float LegacyGradientToDegrees(float LegacyGradient)
    {
        if (!VoxelMath::IsFinite(LegacyGradient) || LegacyGradient <= 0.0f)
        {
            return 0.0f;
        }
        if (LegacyGradient >= 4096.0f)
        {
            return VoxelPassageGeometry::TunnelFloorGentleSlopeMaximumDegrees;
        }

        // Invert the old gradient without atan/tan. The same deterministic sine/cosine helper
        // used by generation makes this migration independent of the host CRT.
        float LowDegrees = 0.0f;
        float HighDegrees = VoxelPassageGeometry::TunnelFloorGentleSlopeMaximumDegrees;
        for (int32 Iteration = 0; Iteration < 32; ++Iteration)
        {
            const float MidDegrees = (LowDegrees + HighDegrees) * 0.5f;
            float SinValue = 0.0f;
            float CosValue = 0.0f;
            VoxelMath::DetSinCos(
                SinValue, CosValue, MidDegrees * (PI / 180.0f));
            const float MidGradient = FMath::Min(
                FMath::Abs(SinValue) / FMath::Max(FMath::Abs(CosValue), 1.0e-3f),
                4096.0f);
            if (MidGradient < LegacyGradient)
            {
                LowDegrees = MidDegrees;
            }
            else
            {
                HighDegrees = MidDegrees;
            }
        }
        return FMath::Clamp(
            (LowDegrees + HighDegrees) * 0.5f,
            0.0f,
            VoxelPassageGeometry::TunnelFloorGentleSlopeMaximumDegrees);
    }
}

void UVoxelStrateDefinition::PostLoad()
{
    Super::PostLoad();

    if (GetLinkerCustomVersion(GVoxelForgeStrateSerializationVersion)
        >= GVoxelForgeStrateGentleSlopeDegreesVersion)
    {
        return;
    }

    const float LegacyGradient = GenerationParams.TunnelFloorGentleSlopeThreshold;
    GenerationParams.TunnelFloorGentleSlopeThreshold =
        LegacyGradientToDegrees(LegacyGradient);
}
