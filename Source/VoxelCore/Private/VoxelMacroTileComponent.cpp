#include "VoxelMacroTileComponent.h"
#include "VoxelSettings.h"
#include "VoxelGenerator.h"
#include "VoxelMesher.h"
#include "VoxelWorld.h"
#include "ProceduralMeshComponent.h"
#include "Async/Async.h"

#if WITH_RUNTIME_MESHCOMPONENT
#include "RuntimeMeshComponent.h"
#endif

// ============================================================================
// INITIALIZATION
// ============================================================================

void UVoxelMacroTileComponent::InitializeMacroTile(
	FIntPoint InTileCoord,
	const UVoxelSettings* InSettings,
	AVoxelWorld* InWorld)
{
	Settings = InSettings;
	OwnerWorld = InWorld;

	// Store tile coords in ChunkCoord for reuse of world helpers
	// (ChunkCoord.Cx/Cy represent tile indices, not actual chunk coordinates)
	ChunkCoord = FVoxelCoord(InTileCoord.X, InTileCoord.Y, 0);

	// Configure as LOD2 heightfield
	LOD = EVoxelLODLevel::LOD2;
	RenderMode = EVoxelRenderMode::Heightfield;
	bBuildCollision = false; // Visual-only (no collision for far ring)
	bUseAO = false;

	// Load macro-tile parameters from settings
	MacroSize = FMath::Max(2, Settings->LOD2_MacroTileSize);
	SampleXY = FMath::Max(1, Settings->LOD2_SampleXY);

	CreateMeshComponent();
	ComputeWorldOrigin();

	// Compute heightmap grid dimensions
	// Each chunk contributes (ChunkSize / SampleXY) + 1 samples
	// Merged tiles share edge samples (hence the -1 adjustment)
	const int32 PerChunkSampX = (Settings->ChunkSizeX + SampleXY - 1) / SampleXY + 1;
	const int32 PerChunkSampY = (Settings->ChunkSizeY + SampleXY - 1) / SampleXY + 1;

	MacroSamplesX = MacroSize * (PerChunkSampX - 1) + 1;
	MacroSamplesY = MacroSize * (PerChunkSampY - 1) + 1;

	// Place mesh component at world origin (vertices will be in world space)
	if (PMC) PMC->SetWorldLocation(WorldOrigin);
#if WITH_RUNTIME_MESHCOMPONENT
	if (RMC) RMC->SetWorldLocation(WorldOrigin);
#endif

	// Schedule generation via world task scheduler
	State = EVoxelChunkState::Generating;
	bCancelPending.AtomicSet(false);
	if (OwnerWorld) OwnerWorld->ScheduleGeneration(this);
}

void UVoxelMacroTileComponent::ComputeWorldOrigin()
{
	const float UU = Settings->VoxelWorldScale;
	const float TileWorldX = MacroSize * Settings->ChunkSizeX * UU;
	const float TileWorldY = MacroSize * Settings->ChunkSizeY * UU;

	WorldOrigin = FVector(
		ChunkCoord.Cx * TileWorldX,
		ChunkCoord.Cy * TileWorldY,
		0.0f);
}

// ============================================================================
// GENERATION (HEIGHTMAP SAMPLING)
// ============================================================================

void UVoxelMacroTileComponent::DoGeneration()
{
	// Build heightfield by sampling generator across macro-tile area
	const AVoxelWorld* W = OwnerWorld;
	const int32 SX = MacroSamplesX;
	const int32 SY = MacroSamplesY;

	TArray<int32> Heights;
	Heights.SetNumUninitialized(SX * SY);

	const float UU = Settings->VoxelWorldScale;

	// World XY step size (spacing between height samples)
	const float StepX = SampleXY * UU;
	const float StepY = SampleXY * UU;

	const FVector Origin = WorldOrigin;

	// Launch async generation task
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, Heights = MoveTemp(Heights), SX, SY, StepX, StepY, Origin]()
		mutable
		{
			TArray<int32> Local = Heights;

			// Sample height at each grid point
			for (int32 y = 0; y < SY; ++y)
			{
				const float Wy = Origin.Y + y * StepY;
				for (int32 x = 0; x < SX; ++x)
				{
					const float Wx = Origin.X + x * StepX;

					// Sample terrain height from generator
					// NOTE: UVoxelGenerator::SampleHeightWorld must be implemented
					const int32 H = UVoxelGenerator::SampleHeightWorld(Wx, Wy);
					Local[x + y * SX] = H;
				}
			}

			// Check for cancellation
			if (bCancelPending)
			{
				AsyncTask(ENamedThreads::GameThread, [this]()
					{
						if (OwnerWorld) OwnerWorld->OnGenerationFinished(this);
					});
				return;
			}

			// Return to game thread with completed heightmap
			AsyncTask(ENamedThreads::GameThread, [this, L = MoveTemp(Local), SX, SY]()
				{
					HeightData = L;
					HF_SamplesX = SX;
					HF_SamplesY = SY;

					State = EVoxelChunkState::Meshing;
					if (OwnerWorld)
					{
						OwnerWorld->OnGenerationFinished(this);
						OwnerWorld->ScheduleMeshing(this, /*bSeamRemesh=*/false);
					}
				});
		});
}

// ============================================================================
// MESHING (HEIGHTFIELD QUAD MESH)
// ============================================================================

void UVoxelMacroTileComponent::DoMeshing(bool /*bSeamRemesh*/)
{
	// Early out if no heightmap data
	if (HeightData.Num() == 0)
	{
		if (OwnerWorld) OwnerWorld->OnMeshingFinished(this);
		return;
	}

	const float VoxelUU = Settings->VoxelWorldScale;

	// Take local copy for thread-safety
	TArray<int32> HeightsCopy = HeightData;
	const int32 SX = HF_SamplesX;
	const int32 SY = HF_SamplesY;

	// Macro geometry size in voxels (chunks × voxels per chunk)
	const int32 MacroChunkSizeX = MacroSize * Settings->ChunkSizeX;
	const int32 MacroChunkSizeY = MacroSize * Settings->ChunkSizeY;

	const int32 XYScale = SampleXY;

	AVoxelWorld* W = OwnerWorld;

	// Launch async meshing task
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, W, Heights = MoveTemp(HeightsCopy), SX, SY, MacroChunkSizeX, MacroChunkSizeY, XYScale, VoxelUU]()
		{
			// Build heightfield mesh from sampled heights
			FMeshBuffers Buffers;
			UVoxelMesher::BuildHeightfieldMesh(Heights, SX, SY, MacroChunkSizeX, MacroChunkSizeY, XYScale, VoxelUU, Buffers);

			if (W)
			{
				W->OnMeshingFinished(this); // Free task slot

				// NOTE: Currently macro-tiles skip mesh apply queue (commented out)
				// Uncomment to enable visual rendering:
				// W->EnqueueMeshApply(this, MoveTemp(Buffers), /*bCreateCollision=*/false, /*bWasSeamRemesh=*/false);
			}
		});
}
