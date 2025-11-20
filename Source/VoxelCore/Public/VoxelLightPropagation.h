#pragma once

#include "CoreMinimal.h"
#include "VoxelStructs.h"

/**
 * Voxel Light Propagation System
 *
 * Implements Minecraft-style flood-fill light propagation for realistic
 * cave lighting that doesn't leak through ungenerated chunks.
 *
 * Algorithm Overview:
 * 1. Sky Light: Starts at 15 on surface, propagates down/horizontally
 * 2. Block Light: Starts at light source (torch=14, lava=15), radiates outward
 * 3. Light decreases by 1 per block distance
 * 4. Solid blocks stop light propagation
 * 5. Semi-solid blocks (water) allow light but reduce it
 *
 * Performance:
 * - Multi-threaded capable
 * - ~0.5-2ms per 32x32x32 chunk
 * - Efficient update queue for dynamic changes
 */

namespace VoxelLighting
{
	/**
	 * Propagate sky light through a chunk using flood-fill algorithm.
	 *
	 * @param LightData     Output light data to populate
	 * @param Categories    Input voxel categories (air/semi-solid/solid)
	 * @param bTopExposed   True if chunk has sky above (not underground)
	 *
	 * IMPORTANT: This only handles vertical propagation within the chunk.
	 * For horizontal propagation across chunk boundaries, see PropagateAcrossChunks().
	 */
	void PropagateSkyLight(FVoxelLightData& LightData, const FCategoryBitset& Categories, bool bTopExposed);

	/**
	 * Propagate block light from a specific source position (torch, lava, etc.).
	 *
	 * @param LightData       Light data to update
	 * @param Categories      Voxel categories
	 * @param SourcePos       Position of light source (local chunk coordinates)
	 * @param SourceStrength  Light level of source (14 for torch, 15 for lava)
	 */
	void PropagateBlockLightFrom(FVoxelLightData& LightData, const FCategoryBitset& Categories,
	                             FIntVector SourcePos, uint8 SourceStrength);

	/**
	 * Remove light from a position and propagate darkness (for block breaking).
	 * This is the inverse of light propagation - we need to un-light areas
	 * when a light source is removed.
	 *
	 * @param LightData    Light data to update
	 * @param Categories   Voxel categories
	 * @param RemovedPos   Position where light was removed
	 * @param bSkyLight    True to remove sky light, false for block light
	 */
	void RemoveLightFrom(FVoxelLightData& LightData, const FCategoryBitset& Categories,
	                     FIntVector RemovedPos, bool bSkyLight);

	/**
	 * Full chunk lighting rebuild (called during initial chunk generation).
	 * Combines sky light and any natural block light sources.
	 *
	 * @param LightData       Output light data
	 * @param Categories      Input voxel categories
	 * @param BlockTypes      Input block types (to detect glowing blocks)
	 * @param bTopExposed     True if chunk has sky above
	 */
	void RebuildChunkLighting(FVoxelLightData& LightData, const FCategoryBitset& Categories,
	                          const TArray<EVoxelBlockID>& BlockTypes, bool bTopExposed);

	/**
	 * Check if a block type emits light (torch, lava, glowstone, etc.).
	 * Returns light strength (0-15), or 0 if block doesn't emit light.
	 */
	FORCEINLINE uint8 GetBlockEmission(EVoxelBlockID BlockType)
	{
		switch (BlockType)
		{
		case EVoxelBlockID::Water:  // Example: glowing water
			return 0;  // Water doesn't emit light by default

		// TODO: Add light-emitting blocks here when implemented
		// case EVoxelBlockID::Torch:     return 14;
		// case EVoxelBlockID::Lava:      return 15;
		// case EVoxelBlockID::Glowstone: return 15;
		// case EVoxelBlockID::Magma:     return 3;

		default:
			return 0;  // No light emission
		}
	}

	/**
	 * Check if a block category allows light to pass through.
	 *
	 * @param Category  Voxel category (0=air, 1=semi-solid, 2=solid)
	 * @return True if light can pass through (air or semi-solid)
	 */
	FORCEINLINE bool IsTransparent(uint8 Category)
	{
		return Category < 2;  // Air (0) and semi-solid (1) allow light
	}

	/**
	 * Get light reduction amount when passing through a block.
	 *
	 * @param Category  Voxel category
	 * @return Light reduction amount (0-15)
	 */
	FORCEINLINE uint8 GetLightReduction(uint8 Category)
	{
		switch (Category)
		{
		case 0:  // Air - no reduction
			return 1;  // Standard 1-per-block reduction
		case 1:  // Semi-solid (water) - slight reduction
			return 1;  // Same as air for now (TODO: could be 2 for murky water)
		case 2:  // Solid - blocks all light
			return 15;  // Effectively stops propagation
		default:
			return 1;
		}
	}
}
