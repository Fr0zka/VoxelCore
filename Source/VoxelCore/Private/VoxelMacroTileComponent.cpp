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

void UVoxelMacroTileComponent::InitializeMacroTile(
    FIntPoint InTileCoord,
    const UVoxelSettings* InSettings,
    AVoxelWorld* InWorld)
{
    Settings = InSettings;
    OwnerWorld = InWorld;

    // Store tile coords inside ChunkCoord for reuse of world helpers
    ChunkCoord = FVoxelCoord(InTileCoord.X, InTileCoord.Y, 0);

    LOD = EVoxelLODLevel::LOD2;
    RenderMode = EVoxelRenderMode::Heightfield;
    bBuildCollision = false; // generally off for far ring
    bUseAO = false;

    MacroSize = FMath::Max(2, Settings->LOD2_MacroTileSize);
    SampleXY = FMath::Max(1, Settings->LOD2_SampleXY);

    CreateMeshComponent();
    ComputeWorldOrigin();

    // Height sample grid:
    // Per chunk samples = (ChunkSize / SampleXY) + 1; merged across tiles sharing edges
    const int32 PerChunkSampX = (Settings->ChunkSizeX + SampleXY - 1) / SampleXY + 1;
    const int32 PerChunkSampY = (Settings->ChunkSizeY + SampleXY - 1) / SampleXY + 1;

    MacroSamplesX = MacroSize * (PerChunkSampX - 1) + 1;
    MacroSamplesY = MacroSize * (PerChunkSampY - 1) + 1;

    // Place component at world origin for transform-less verts
    if (PMC) PMC->SetWorldLocation(WorldOrigin);
#if WITH_RUNTIME_MESHCOMPONENT
    if (RMC) RMC->SetWorldLocation(WorldOrigin);
#endif

    // Kick generation via scheduler
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

void UVoxelMacroTileComponent::DoGeneration()
{
    // We’ll build a single heightfield by sampling the generator
    // across the macro-tile area at SampleXY spacing.
    const AVoxelWorld* W = OwnerWorld;
    const int32 SX = MacroSamplesX;
    const int32 SY = MacroSamplesY;

    TArray<int32> Heights;
    Heights.SetNumUninitialized(SX * SY);

    const float UU = Settings->VoxelWorldScale;

    // World XY to "voxel" XY steps
    const float StepX = SampleXY * UU;
    const float StepY = SampleXY * UU;

    const FVector Origin = WorldOrigin;

    // Sample the generator height at each grid node.
    // We rely on UVoxelGenerator::SampleHeightWorldXY (add in Generator if missing),
    // else call GenerateHeightmap per chunk and stitch (slower). Here, we sample directly.
    UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, Heights = MoveTemp(Heights), SX, SY, StepX, StepY, Origin]()
        mutable
        {
            TArray<int32> Local = Heights;

            for (int32 y = 0; y < SY; ++y)
            {
                const float Wy = Origin.Y + y * StepY;
                for (int32 x = 0; x < SX; ++x)
                {
                    const float Wx = Origin.X + x * StepX;

                    // NOTE: Implement this static in your generator: int32 UVoxelGenerator::SampleHeightWorld(float WX, float WY)
                    // For now, we’ll reconstruct chunk-local query via params if you already have that path.
                    const int32 H = UVoxelGenerator::SampleHeightWorld(Wx, Wy);
                    Local[x + y * SX] = H;
                }
            }

            if (bCancelPending)
            {
                AsyncTask(ENamedThreads::GameThread, [this]()
                    {
                        if (OwnerWorld) OwnerWorld->OnGenerationFinished(this);
                    });
                return;
            }

            AsyncTask(ENamedThreads::GameThread, [this, L = MoveTemp(Local), SX, SY]()
                {
                    HeightData = L;
                    HF_SamplesX = SX;
                    HF_SamplesY = SY;

                    State = EVoxelChunkState::Meshing;
                    if (OwnerWorld) { OwnerWorld->OnGenerationFinished(this); OwnerWorld->ScheduleMeshing(this, /*bSeamRemesh=*/false); }
                });
        });
}

void UVoxelMacroTileComponent::DoMeshing(bool /*bSeamRemesh*/)
{
    if (HeightData.Num() == 0) { if (OwnerWorld) OwnerWorld->OnMeshingFinished(this); return; }

    const float VoxelUU = Settings->VoxelWorldScale;

    // Take a local copy for thread-safety
    TArray<int32> HeightsCopy = HeightData;
    const int32 SX = HF_SamplesX;
    const int32 SY = HF_SamplesY;

    // Macro geom size in *chunks*
    const int32 MacroChunkSizeX = MacroSize * Settings->ChunkSizeX;
    const int32 MacroChunkSizeY = MacroSize * Settings->ChunkSizeY;

    const int32 XYScale = SampleXY;

    AVoxelWorld* W = OwnerWorld;

    UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, W, Heights = MoveTemp(HeightsCopy), SX, SY, MacroChunkSizeX, MacroChunkSizeY, XYScale, VoxelUU]()
        {
            FMeshBuffers Buffers;
            UVoxelMesher::BuildHeightfieldMesh(Heights, SX, SY, MacroChunkSizeX, MacroChunkSizeY, XYScale, VoxelUU, Buffers);

            if (W)
            {
                W->OnMeshingFinished(this); // free slot
                // No collision for macro-tiles (cheap apply)
               // W->EnqueueMeshApply(this, MoveTemp(Buffers), /*bCreateCollision=*/false, /*bWasSeamRemesh=*/false);
            }
        });
}
