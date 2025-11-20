#include "VoxelLightPropagation.h"
#include "VoxelStats.h"
#include "Containers/Queue.h"

namespace VoxelLighting
{
	// Helper: Check if position is valid within chunk bounds
	static FORCEINLINE bool IsValidPos(const FIntVector& Pos, int32 SizeX, int32 SizeY, int32 SizeZ)
	{
		return Pos.X >= 0 && Pos.X < SizeX &&
		       Pos.Y >= 0 && Pos.Y < SizeY &&
		       Pos.Z >= 0 && Pos.Z < SizeZ;
	}

	// Helper: Get 6 neighbors (up, down, north, south, east, west)
	static const FIntVector Neighbors[6] = {
		FIntVector(1, 0, 0),   // +X
		FIntVector(-1, 0, 0),  // -X
		FIntVector(0, 1, 0),   // +Y
		FIntVector(0, -1, 0),  // -Y
		FIntVector(0, 0, 1),   // +Z (up)
		FIntVector(0, 0, -1)   // -Z (down)
	};

	void PropagateSkyLight(FVoxelLightData& LightData, const FCategoryBitset& Categories, bool bTopExposed)
	{
		SCOPE_CYCLE_COUNTER(STAT_VoxelLightPropagate);

		// Clear existing light
		LightData.Clear();

		if (!bTopExposed)
		{
			// Underground chunk - no sky light
			return;
		}

		const int32 SizeX = LightData.SizeX;
		const int32 SizeY = LightData.SizeY;
		const int32 SizeZ = LightData.SizeZ;

		// Queue for flood-fill (stores position + light level)
		TQueue<TPair<FIntVector, uint8>> Queue;

		// PHASE 1: Initialize top surface with sky light = 15
		const int32 TopZ = SizeZ - 1;
		for (int32 X = 0; X < SizeX; ++X)
		{
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				// Start from top, find first non-air block or go all the way down
				for (int32 Z = TopZ; Z >= 0; --Z)
				{
					const uint8 Cat = Categories.Get(X, Y, Z);

					if (Cat == 0)  // Air
					{
						// Set full sky light for air blocks at top
						LightData.SetSkyLight(X, Y, Z, 15);

						// Only add to queue at the bottom of air column
						// (propagation will handle the rest)
						if (Z == 0 || Categories.Get(X, Y, Z - 1) != 0)
						{
							Queue.Enqueue(TPair<FIntVector, uint8>(FIntVector(X, Y, Z), 15));
						}
					}
					else
					{
						// Hit solid/semi-solid block, stop vertical propagation for this column
						// But if it's semi-solid (water), we still set reduced light
						if (Cat == 1)  // Semi-solid
						{
							LightData.SetSkyLight(X, Y, Z, 14);  // Reduced by 1
							Queue.Enqueue(TPair<FIntVector, uint8>(FIntVector(X, Y, Z), 14));
						}
						break;  // Don't go deeper in this column
					}
				}
			}
		}

		// PHASE 2: Flood-fill horizontally and downward
		while (!Queue.IsEmpty())
		{
			TPair<FIntVector, uint8> Item;
			Queue.Dequeue(Item);

			const FIntVector& Pos = Item.Key;
			const uint8 CurrentLight = Item.Value;

			if (CurrentLight == 0)
				continue;  // No light to propagate

			// Calculate propagated light (reduce by 1)
			const uint8 PropagatedLight = CurrentLight - 1;

			// Check all 6 neighbors
			for (int32 i = 0; i < 6; ++i)
			{
				const FIntVector NeighborPos = Pos + Neighbors[i];

				// Bounds check
				if (!IsValidPos(NeighborPos, SizeX, SizeY, SizeZ))
					continue;

				// Check if neighbor blocks light
				const uint8 NeighborCat = Categories.Get(NeighborPos.X, NeighborPos.Y, NeighborPos.Z);
				if (NeighborCat == 2)  // Solid block
					continue;  // Light blocked

				// Get current light at neighbor
				const uint8 NeighborLight = LightData.GetSkyLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z);

				// Only propagate if we would increase the light level
				if (PropagatedLight > NeighborLight)
				{
					LightData.SetSkyLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z, PropagatedLight);
					Queue.Enqueue(TPair<FIntVector, uint8>(NeighborPos, PropagatedLight));
				}
			}
		}
	}

	void PropagateBlockLightFrom(FVoxelLightData& LightData, const FCategoryBitset& Categories,
	                             FIntVector SourcePos, uint8 SourceStrength)
	{
		SCOPE_CYCLE_COUNTER(STAT_VoxelLightPropagateBlock);

		const int32 SizeX = LightData.SizeX;
		const int32 SizeY = LightData.SizeY;
		const int32 SizeZ = LightData.SizeZ;

		// Validate source position
		if (!IsValidPos(SourcePos, SizeX, SizeY, SizeZ))
			return;

		// Clamp source strength
		SourceStrength = FMath::Clamp(SourceStrength, (uint8)0, (uint8)15);

		// Set light at source
		LightData.SetBlockLight(SourcePos.X, SourcePos.Y, SourcePos.Z, SourceStrength);

		// Queue for flood-fill
		TQueue<TPair<FIntVector, uint8>> Queue;
		Queue.Enqueue(TPair<FIntVector, uint8>(SourcePos, SourceStrength));

		// Flood-fill algorithm (same as sky light but for block light)
		while (!Queue.IsEmpty())
		{
			TPair<FIntVector, uint8> Item;
			Queue.Dequeue(Item);

			const FIntVector& Pos = Item.Key;
			const uint8 CurrentLight = Item.Value;

			if (CurrentLight == 0)
				continue;

			const uint8 PropagatedLight = CurrentLight - 1;

			// Check all 6 neighbors
			for (int32 i = 0; i < 6; ++i)
			{
				const FIntVector NeighborPos = Pos + Neighbors[i];

				if (!IsValidPos(NeighborPos, SizeX, SizeY, SizeZ))
					continue;

				const uint8 NeighborCat = Categories.Get(NeighborPos.X, NeighborPos.Y, NeighborPos.Z);
				if (NeighborCat == 2)  // Solid
					continue;

				const uint8 NeighborLight = LightData.GetBlockLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z);

				if (PropagatedLight > NeighborLight)
				{
					LightData.SetBlockLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z, PropagatedLight);
					Queue.Enqueue(TPair<FIntVector, uint8>(NeighborPos, PropagatedLight));
				}
			}
		}
	}

	void RemoveLightFrom(FVoxelLightData& LightData, const FCategoryBitset& Categories,
	                     FIntVector RemovedPos, bool bSkyLight)
	{
		SCOPE_CYCLE_COUNTER(STAT_VoxelLightRemove);

		const int32 SizeX = LightData.SizeX;
		const int32 SizeY = LightData.SizeY;
		const int32 SizeZ = LightData.SizeZ;

		if (!IsValidPos(RemovedPos, SizeX, SizeY, SizeZ))
			return;

		// Two-phase algorithm:
		// Phase 1: Remove light (flood-fill darkness)
		// Phase 2: Re-propagate light from neighboring sources

		TQueue<FIntVector> RemovalQueue;
		TQueue<TPair<FIntVector, uint8>> RepropagateQueue;

		// Get initial light value at removed position
		const uint8 RemovedLight = bSkyLight
			? LightData.GetSkyLight(RemovedPos.X, RemovedPos.Y, RemovedPos.Z)
			: LightData.GetBlockLight(RemovedPos.X, RemovedPos.Y, RemovedPos.Z);

		// Clear light at source
		if (bSkyLight)
			LightData.SetSkyLight(RemovedPos.X, RemovedPos.Y, RemovedPos.Z, 0);
		else
			LightData.SetBlockLight(RemovedPos.X, RemovedPos.Y, RemovedPos.Z, 0);

		RemovalQueue.Enqueue(RemovedPos);

		// PHASE 1: Remove light
		while (!RemovalQueue.IsEmpty())
		{
			FIntVector Pos;
			RemovalQueue.Dequeue(Pos);

			// Check all neighbors
			for (int32 i = 0; i < 6; ++i)
			{
				const FIntVector NeighborPos = Pos + Neighbors[i];

				if (!IsValidPos(NeighborPos, SizeX, SizeY, SizeZ))
					continue;

				const uint8 NeighborLight = bSkyLight
					? LightData.GetSkyLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z)
					: LightData.GetBlockLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z);

				if (NeighborLight == 0)
					continue;  // Already dark

				if (NeighborLight < RemovedLight)
				{
					// This neighbor was lit by the removed source - remove its light too
					if (bSkyLight)
						LightData.SetSkyLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z, 0);
					else
						LightData.SetBlockLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z, 0);

					RemovalQueue.Enqueue(NeighborPos);
				}
				else
				{
					// This neighbor has light from another source - re-propagate from it
					RepropagateQueue.Enqueue(TPair<FIntVector, uint8>(NeighborPos, NeighborLight));
				}
			}
		}

		// PHASE 2: Re-propagate light from remaining sources
		while (!RepropagateQueue.IsEmpty())
		{
			TPair<FIntVector, uint8> Item;
			RepropagateQueue.Dequeue(Item);

			const FIntVector& Pos = Item.Key;
			const uint8 CurrentLight = Item.Value;

			if (CurrentLight == 0)
				continue;

			const uint8 PropagatedLight = CurrentLight - 1;

			for (int32 i = 0; i < 6; ++i)
			{
				const FIntVector NeighborPos = Pos + Neighbors[i];

				if (!IsValidPos(NeighborPos, SizeX, SizeY, SizeZ))
					continue;

				const uint8 NeighborCat = Categories.Get(NeighborPos.X, NeighborPos.Y, NeighborPos.Z);
				if (NeighborCat == 2)
					continue;

				const uint8 NeighborLight = bSkyLight
					? LightData.GetSkyLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z)
					: LightData.GetBlockLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z);

				if (PropagatedLight > NeighborLight)
				{
					if (bSkyLight)
						LightData.SetSkyLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z, PropagatedLight);
					else
						LightData.SetBlockLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z, PropagatedLight);

					RepropagateQueue.Enqueue(TPair<FIntVector, uint8>(NeighborPos, PropagatedLight));
				}
			}
		}
	}

	void RebuildChunkLighting(FVoxelLightData& LightData, const FCategoryBitset& Categories,
	                          const TArray<EVoxelBlockID>& BlockTypes, bool bTopExposed)
	{
		SCOPE_CYCLE_COUNTER(STAT_VoxelLightRebuild);

		// Step 1: Propagate sky light
		PropagateSkyLight(LightData, Categories, bTopExposed);

		// Step 2: Find and propagate block light sources
		const int64 TotalVoxels = static_cast<int64>(LightData.SizeX) * LightData.SizeY * LightData.SizeZ;

		for (int64 i = 0; i < TotalVoxels && i < BlockTypes.Num(); ++i)
		{
			const uint8 Emission = GetBlockEmission(BlockTypes[i]);

			if (Emission > 0)
			{
				// Convert linear index to 3D coordinates
				const int32 X = i % LightData.SizeX;
				const int32 Y = (i / LightData.SizeX) % LightData.SizeY;
				const int32 Z = i / (LightData.SizeX * LightData.SizeY);

				// Propagate light from this source
				PropagateBlockLightFrom(LightData, Categories, FIntVector(X, Y, Z), Emission);
			}
		}
	}

	// CROSS-CHUNK PROPAGATION: Extract boundary light values
	void ExtractBoundaryLight(
		const FVoxelLightData& LightData,
		TArray<uint8>& OutXNeg, TArray<uint8>& OutXPos,
		TArray<uint8>& OutYNeg, TArray<uint8>& OutYPos,
		TArray<uint8>& OutZNeg, TArray<uint8>& OutZPos)
	{
		const int32 SX = LightData.SizeX;
		const int32 SY = LightData.SizeY;
		const int32 SZ = LightData.SizeZ;

		// Resize output arrays
		OutXNeg.SetNumZeroed(SY * SZ);
		OutXPos.SetNumZeroed(SY * SZ);
		OutYNeg.SetNumZeroed(SX * SZ);
		OutYPos.SetNumZeroed(SX * SZ);
		OutZNeg.SetNumZeroed(SX * SY);
		OutZPos.SetNumZeroed(SX * SY);

		// Extract X faces
		for (int32 z = 0; z < SZ; ++z)
		{
			for (int32 y = 0; y < SY; ++y)
			{
				OutXNeg[y + z * SY] = LightData.GetCombinedLight(0, y, z);
				OutXPos[y + z * SY] = LightData.GetCombinedLight(SX - 1, y, z);
			}
		}

		// Extract Y faces
		for (int32 z = 0; z < SZ; ++z)
		{
			for (int32 x = 0; x < SX; ++x)
			{
				OutYNeg[x + z * SX] = LightData.GetCombinedLight(x, 0, z);
				OutYPos[x + z * SX] = LightData.GetCombinedLight(x, SY - 1, z);
			}
		}

		// Extract Z faces
		for (int32 y = 0; y < SY; ++y)
		{
			for (int32 x = 0; x < SX; ++x)
			{
				OutZNeg[x + y * SX] = LightData.GetCombinedLight(x, y, 0);
				OutZPos[x + y * SX] = LightData.GetCombinedLight(x, y, SZ - 1);
			}
		}
	}

	// CROSS-CHUNK PROPAGATION: Apply boundary light and propagate inward
	void ApplyBoundaryLightFromNeighbors(
		FVoxelLightData& LightData,
		const FCategoryBitset& Categories,
		const TArray<uint8>& InXNeg, const TArray<uint8>& InXPos,
		const TArray<uint8>& InYNeg, const TArray<uint8>& InYPos,
		const TArray<uint8>& InZNeg, const TArray<uint8>& InZPos)
	{
		const int32 SX = LightData.SizeX;
		const int32 SY = LightData.SizeY;
		const int32 SZ = LightData.SizeZ;

		TQueue<TPair<FIntVector, uint8>> PropagationQueue;

		// Helper to apply boundary light at a position
		auto ApplyBoundary = [&](int32 x, int32 y, int32 z, uint8 IncomingLight)
		{
			if (IncomingLight == 0) return;

			// Check if this voxel blocks light
			const uint8 Cat = Categories.Get(x, y, z);
			if (Cat == 2) return;  // Solid - blocks light

			// Get current light at this position
			const uint8 CurrentLight = LightData.GetCombinedLight(x, y, z);

			// Only apply if incoming light is brighter
			if (IncomingLight > CurrentLight)
			{
				// Split into sky and block light (for now, assume it's all sky light)
				// TODO: Later we can store sky/block separately in boundary arrays
				LightData.SetSkyLight(x, y, z, IncomingLight);
				PropagationQueue.Enqueue(TPair<FIntVector, uint8>(FIntVector(x, y, z), IncomingLight));
			}
		};

		// Apply X faces
		if (InXNeg.Num() == SY * SZ)
		{
			for (int32 z = 0; z < SZ; ++z)
			{
				for (int32 y = 0; y < SY; ++y)
				{
					const uint8 Light = InXNeg[y + z * SY];
					if (Light > 1) ApplyBoundary(0, y, z, Light - 1);  // Reduce by 1 per block distance
				}
			}
		}
		else
		{
			// No neighbor: this face borders the void, so it should be dark
			for (int32 z = 0; z < SZ; ++z)
			{
				for (int32 y = 0; y < SY; ++y)
				{
					LightData.SetSkyLight(0, y, z, 0);
					LightData.SetBlockLight(0, y, z, 0);
				}
			}
		}
		if (InXPos.Num() == SY * SZ)
		{
			for (int32 z = 0; z < SZ; ++z)
			{
				for (int32 y = 0; y < SY; ++y)
				{
					const uint8 Light = InXPos[y + z * SY];
					if (Light > 1) ApplyBoundary(SX - 1, y, z, Light - 1);
				}
			}
		}
		else
		{
			// No neighbor: this face borders the void, so it should be dark
			for (int32 z = 0; z < SZ; ++z)
			{
				for (int32 y = 0; y < SY; ++y)
				{
					LightData.SetSkyLight(SX - 1, y, z, 0);
					LightData.SetBlockLight(SX - 1, y, z, 0);
				}
			}
		}

		// Apply Y faces
		if (InYNeg.Num() == SX * SZ)
		{
			for (int32 z = 0; z < SZ; ++z)
			{
				for (int32 x = 0; x < SX; ++x)
				{
					const uint8 Light = InYNeg[x + z * SX];
					if (Light > 1) ApplyBoundary(x, 0, z, Light - 1);
				}
			}
		}
		else
		{
			// No neighbor: this face borders the void, so it should be dark
			for (int32 z = 0; z < SZ; ++z)
			{
				for (int32 x = 0; x < SX; ++x)
				{
					LightData.SetSkyLight(x, 0, z, 0);
					LightData.SetBlockLight(x, 0, z, 0);
				}
			}
		}
		if (InYPos.Num() == SX * SZ)
		{
			for (int32 z = 0; z < SZ; ++z)
			{
				for (int32 x = 0; x < SX; ++x)
				{
					const uint8 Light = InYPos[x + z * SX];
					if (Light > 1) ApplyBoundary(x, SY - 1, z, Light - 1);
				}
			}
		}
		else
		{
			// No neighbor: this face borders the void, so it should be dark
			for (int32 z = 0; z < SZ; ++z)
			{
				for (int32 x = 0; x < SX; ++x)
				{
					LightData.SetSkyLight(x, SY - 1, z, 0);
					LightData.SetBlockLight(x, SY - 1, z, 0);
				}
			}
		}

		// Apply Z faces
		if (InZNeg.Num() == SX * SY)
		{
			for (int32 y = 0; y < SY; ++y)
			{
				for (int32 x = 0; x < SX; ++x)
				{
					const uint8 Light = InZNeg[x + y * SX];
					if (Light > 1) ApplyBoundary(x, y, 0, Light - 1);
				}
			}
		}
		else
		{
			// No neighbor: this face borders the void, so it should be dark
			for (int32 y = 0; y < SY; ++y)
			{
				for (int32 x = 0; x < SX; ++x)
				{
					LightData.SetSkyLight(x, y, 0, 0);
					LightData.SetBlockLight(x, y, 0, 0);
				}
			}
		}
		if (InZPos.Num() == SX * SY)
		{
			for (int32 y = 0; y < SY; ++y)
			{
				for (int32 x = 0; x < SX; ++x)
				{
					const uint8 Light = InZPos[x + y * SX];
					if (Light > 1) ApplyBoundary(x, y, SZ - 1, Light - 1);
				}
			}
		}
		else
		{
			// No neighbor: this face borders the void, so it should be dark
			for (int32 y = 0; y < SY; ++y)
			{
				for (int32 x = 0; x < SX; ++x)
				{
					LightData.SetSkyLight(x, y, SZ - 1, 0);
					LightData.SetBlockLight(x, y, SZ - 1, 0);
				}
			}
		}

		// Propagate the boundary light inward using flood-fill
		while (!PropagationQueue.IsEmpty())
		{
			TPair<FIntVector, uint8> Item;
			PropagationQueue.Dequeue(Item);

			const FIntVector& Pos = Item.Key;
			const uint8 CurrentLight = Item.Value;

			if (CurrentLight <= 1) continue;

			const uint8 PropagatedLight = CurrentLight - 1;

			// Check all 6 neighbors
			for (int32 i = 0; i < 6; ++i)
			{
				const FIntVector NeighborPos = Pos + Neighbors[i];

				if (!IsValidPos(NeighborPos, SX, SY, SZ))
					continue;

				const uint8 NeighborCat = Categories.Get(NeighborPos.X, NeighborPos.Y, NeighborPos.Z);
				if (NeighborCat == 2) continue;

				const uint8 NeighborLight = LightData.GetCombinedLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z);

				if (PropagatedLight > NeighborLight)
				{
					LightData.SetSkyLight(NeighborPos.X, NeighborPos.Y, NeighborPos.Z, PropagatedLight);
					PropagationQueue.Enqueue(TPair<FIntVector, uint8>(NeighborPos, PropagatedLight));
				}
			}
		}
	}
}
