#include "VoxelChunkComponent.h"
#include "VoxelSettings.h"
#include "VoxelGenerator.h"
#include "VoxelMesher.h"
#include "VoxelWorld.h"
#include "ProceduralMeshComponent.h"
#include "Async/Async.h"

#if WITH_RUNTIME_MESHCOMPONENT
#include "RuntimeMeshComponent.h"
#endif

void UVoxelChunkComponent::InitializeChunk(
    FVoxelCoord InCoord,
    const UVoxelSettings* InSettings,
    AVoxelWorld* InWorld,
    EVoxelLODLevel InLOD,
    bool bInBuildCollision,
    bool bInUseAO)
{
    ChunkCoord = InCoord;
    Settings = InSettings;
    OwnerWorld = InWorld;
    LOD = InLOD;
    bBuildCollision = bInBuildCollision;
    bUseAO = bInUseAO;

    LODScaleXY = (LOD == EVoxelLODLevel::LOD1) ? FMath::Max(1, Settings->LOD1_ScaleXY) : 1;
    RenderMode = (LOD == EVoxelLODLevel::LOD2) ? EVoxelRenderMode::Heightfield : EVoxelRenderMode::Voxels;

    CreateMeshComponent();

    const FVector WorldLocation(
        ChunkCoord.Cx * Settings->ChunkSizeX * Settings->VoxelWorldScale,
        ChunkCoord.Cy * Settings->ChunkSizeY * Settings->VoxelWorldScale,
        0.0f);
    if (PMC) PMC->SetWorldLocation(WorldLocation);
#if WITH_RUNTIME_MESHCOMPONENT
    if (RMC) RMC->SetWorldLocation(WorldLocation);
#endif
    if (LOD == EVoxelLODLevel::LOD0)
    {
        PMC->SetCastShadow(true);
    }
    else
    {
        PMC->SetCastShadow(false);
    }
    StartGeneration();
}

void UVoxelChunkComponent::CreateMeshComponent()
{
    DestroyMeshComponent();
    bUsingRMC = false;

    if (!OwnerWorld) return;

#if WITH_RUNTIME_MESHCOMPONENT
    if (Settings->bUseRuntimeMeshComponent)
    {
        RMC = OwnerWorld->AcquireRMC();
        if (RMC) { bUsingRMC = true; return; }
    }
#endif

    PMC = OwnerWorld->AcquirePMC();
}

void UVoxelChunkComponent::DestroyMeshComponent()
{
    if (!OwnerWorld) return;

    if (PMC) { OwnerWorld->ReleasePMC(PMC); PMC = nullptr; }
#if WITH_RUNTIME_MESHCOMPONENT
    if (RMC) { OwnerWorld->ReleaseRMC(RMC); RMC = nullptr; }
#endif
}

void UVoxelChunkComponent::StartGeneration()
{
    State = EVoxelChunkState::Generating;
    bCancelPending.AtomicSet(false);
    if (OwnerWorld) OwnerWorld->ScheduleGeneration(this);
}

void UVoxelChunkComponent::DoGeneration()
{
    const FChunkGenParams Params = UVoxelGenerator::MakeParamsFromSettings(Settings);
    const FVoxelCoord Coord = ChunkCoord;
    const int32 ScaleXY = LODScaleXY;
    const bool bHeight = (RenderMode == EVoxelRenderMode::Heightfield);

    UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, Params, Coord, ScaleXY, bHeight]()
        {
            if (bHeight)
            {
                FIntPoint Samples;
                UVoxelGenerator::GenerateHeightmap(Coord, Params, ScaleXY, HeightData, Samples);
                HF_SamplesX = Samples.X;
                HF_SamplesY = Samples.Y;
            }
            else
            {
                FIntVector Size;
                UVoxelGenerator::GenerateChunkLOD(Coord, Params, ScaleXY, VoxelData, Size);
            }

            if (bCancelPending)
            {
                AsyncTask(ENamedThreads::GameThread, [this]()
                    {
                        if (OwnerWorld) OwnerWorld->OnGenerationFinished(this);
                    });
                return;
            }

            AsyncTask(ENamedThreads::GameThread, [this]()
                {
                    OnGenerationComplete();
                    if (OwnerWorld) OwnerWorld->OnGenerationFinished(this);
                });
        });
}

void UVoxelChunkComponent::OnGenerationComplete()
{
    if (bCancelPending)
    {
        State = EVoxelChunkState::Unloading;
        return;
    }
    StartMeshing(/*bSeamRemesh=*/false);
}

void UVoxelChunkComponent::StartMeshing(bool bSeamRemesh)
{
    if (OwnerWorld) OwnerWorld->ScheduleMeshing(this, bSeamRemesh);
}

void UVoxelChunkComponent::DoMeshing(bool bSeamRemesh)
{
    const bool bIsVox = (RenderMode == EVoxelRenderMode::Voxels);

    // Early outs per mode
    if (bIsVox)
    {
        // Ensure compact storage for both LOD0 and LOD1
        const bool wantCompact = (LOD == EVoxelLODLevel::LOD0 || LOD == EVoxelLODLevel::LOD1);
        if (wantCompact && Compact.Occupancy.Num() == 0)
            ConvertDenseToCompact(/*bForLOD0=*/LOD == EVoxelLODLevel::LOD0);

        const bool empty =
            (Compact.Occupancy.Num() == 0 && VoxelData.Num() == 0);
        if (empty) { if (OwnerWorld) OwnerWorld->OnMeshingFinished(this); return; }
    }
    else
    {
        if (HeightData.Num() == 0) { if (OwnerWorld) OwnerWorld->OnMeshingFinished(this); return; }
    }

    State = EVoxelChunkState::Meshing;
    bIsMeshing = true;
    bCancelPending.AtomicSet(false);

    const float VoxelUU = Settings->VoxelWorldScale;

    // Neighbor halo for seam/AO on LOD0 only
    FChunkNeighbors Nbh;
    if (bIsVox && LOD == EVoxelLODLevel::LOD0) { SnapshotNeighbors(Nbh); }

    // Copy inputs for task
    TArray<EVoxelBlockID> VoxelsCopy;
    TArray<int32> HeightsCopy;
    FCompactVoxelData CompactCopy = Compact;
    FChunkNeighbors NbhCopy = Nbh;

    if (!bIsVox) HeightsCopy = HeightData;
    else if (LOD == EVoxelLODLevel::LOD0 && VoxelData.Num() > 0) VoxelsCopy = VoxelData; // if you ever keep dense

    const bool bCollision = bBuildCollision;
    const bool bAO = bUseAO && bIsVox && (LOD == EVoxelLODLevel::LOD0);
    AVoxelWorld* W = OwnerWorld;

    const FIntVector SizeVox(
        (Settings->ChunkSizeX + LODScaleXY - 1) / LODScaleXY,
        (Settings->ChunkSizeY + LODScaleXY - 1) / LODScaleXY,
        Settings->ChunkSizeZ);

    const int32 ChunkSizeX = Settings->ChunkSizeX;
    const int32 ChunkSizeY = Settings->ChunkSizeY;
    const int32 XYScale = LODScaleXY;

    UE::Tasks::Launch(UE_SOURCE_LOCATION,
        [this, VoxelUU, bCollision, bAO, bSeamRemesh, W,
        SizeVox, ChunkSizeX, ChunkSizeY, XYScale,
        Voxels = MoveTemp(VoxelsCopy),
        HCopy = MoveTemp(HeightsCopy),
        CompactCopy,
        NbhCopy,
        RenderMode = this->RenderMode,
        LOD = this->LOD]() mutable
        {
            FMeshBuffers Buffers;

            if (RenderMode == EVoxelRenderMode::Voxels)
            {
                if (LOD == EVoxelLODLevel::LOD0)
                {
                    // Sparse LOD0 with AO and halos
                    UVoxelMesher::BuildGreedyMesh_FromBitset_AO(
                        CompactCopy, &NbhCopy, VoxelUU, /*bUseAO*/ bAO, Buffers);
                }
                else // LOD1
                {
                    // Fast LOD1 without AO
                    UVoxelMesher::BuildGreedyMesh_FromBitset_AO(
                        CompactCopy, /*Nbh*/nullptr, VoxelUU, /*bUseAO*/ false, Buffers);
                }
            }
            else
            {
                // Your existing heightfield path
                UVoxelMesher::BuildHeightfieldMesh(
                    HCopy, /*SamplesX*/HF_SamplesX, /*SamplesY*/HF_SamplesY,
                    ChunkSizeX, ChunkSizeY, XYScale, VoxelUU, Buffers);
            }

            if (W)
            {
                W->OnMeshingFinished(this);
                W->EnqueueMeshApply(this, MoveTemp(Buffers), bCollision, bSeamRemesh);
            }
        });
}



void UVoxelChunkComponent::ApplyBuffersToMesh(const FMeshBuffers& Bufs, bool bCollision)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Voxel_Chunk_ApplyBuffersToMesh);
    SCOPE_CYCLE_COUNTER(STAT_Voxel_Chunk_ApplyMesh);

    const int32 S = MeshSectionIndex;

    // --- small helper: fast hash of index buffer (detect topology changes even if count is same)
    auto HashIndices = [](const TArray<int32>& A)->uint32
        {
            uint32 h = 2166136261u;                 // FNV-1a 32-bit
            for (int32 v : A) { h ^= (uint32)v; h *= 16777619u; }
            return h;
        };

    const int32  NewV = Bufs.Vertices.Num();
    const int32  NewI = Bufs.Triangles.Num();
    const uint32 NewH = HashIndices(Bufs.Triangles);

    // --- recreate decision (section missing, counts changed, or topology changed)
    auto NeedRecreate = [&]() -> bool
        {
            if (!bSectionCreated)          return true;
            if (NewV != LastVertCount)     return true;
            if (NewI != LastIndexCount)    return true;
            if (NewH != LastIndexHash)     return true; // NEW
            return false;
        };

    // --- update state after (re)create
    auto AfterCreate = [&](bool bCreatedWithCollision)
        {
            bSectionCreated = true;
            bSectionHasCollision = bCreatedWithCollision;
            LastVertCount = NewV;
            LastIndexCount = NewI;
            LastIndexHash = NewH;   // NEW
        };

    // --- update state after "Update" (indices unchanged, but we still track hash)
    auto AfterUpdate = [&]()
        {
            LastVertCount = NewV;
            LastIndexHash = NewH;        // NEW
            // LastIndexCount stays as-is; Update doesn't change indices
        };

    // --- empty mesh => clear section & reset
    if (NewI == 0 || NewV == 0)
    {
        if (bUsingRMC)
        {
#if WITH_RUNTIME_MESHCOMPONENT
            if (RMC && bSectionCreated)
            {
                RMC->ClearSection(0, S);
            }
#endif
        }
        else
        {
            if (PMC && bSectionCreated)
            {
                PMC->ClearMeshSection(S);
            }
        }
        bSectionCreated = false;
        bSectionHasCollision = false;
        LastVertCount = LastIndexCount = 0;
        LastIndexHash = 0;
        return;
    }

    // --- ensure base component flags (cheap)
    if (bUsingRMC)
    {
#if WITH_RUNTIME_MESHCOMPONENT
        check(RMC);
        RMC->SetVisibility(true, true);
        RMC->SetHiddenInGame(false, true);
        RMC->SetCollisionEnabled(ECollisionEnabled::NoCollision); // toggle after
#endif
    }
    else
    {
        check(PMC);
        PMC->bUseAsyncCooking = true;
        PMC->SetVisibility(true, true);
        PMC->SetHiddenInGame(false, true);
        PMC->SetCollisionEnabled(ECollisionEnabled::NoCollision); // toggle after
    }

    const bool bRecreate = NeedRecreate();

    if (bUsingRMC)
    {
#if WITH_RUNTIME_MESHCOMPONENT
        if (bRecreate)
        {
            UVoxelMesher::ApplyToRMC_Create(RMC, S, Bufs, /*bCreateCollision=*/bCollision);
            AfterCreate(bCollision);
        }
        else
        {
            UVoxelMesher::ApplyToRMC_Update(RMC, S, Bufs);
            AfterUpdate();
        }

        // collision policy (upgrade once if needed; otherwise just toggle component)
        if (bCollision)
        {
            if (!bSectionHasCollision && !bRecreate)
            {
                UVoxelMesher::ApplyToRMC_Create(RMC, S, Bufs, /*bCreateCollision=*/true);
                bSectionHasCollision = true;
            }
            RMC->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        }
        else
        {
            RMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        }
#endif
    }
    else
    {
        if (bRecreate)
        {
            UVoxelMesher::ApplyToPMC_Create(PMC, S, Bufs, /*bCreateCollision=*/bCollision);
            AfterCreate(bCollision);
        }
        else
        {
            UVoxelMesher::ApplyToPMC_Update(PMC, S, Bufs);
            AfterUpdate();
        }

        if (bCollision)
        {
            if (!bSectionHasCollision && !bRecreate)
            {
                UVoxelMesher::ApplyToPMC_Create(PMC, S, Bufs, /*bCreateCollision=*/true);
                bSectionHasCollision = true;
            }
            PMC->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        }
        else
        {
            PMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        }
    }
}



void UVoxelChunkComponent::OnMeshApplied(TUniquePtr<FMeshBuffers>&& AppliedBuffers, bool bWasSeamRemesh)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Voxel_Chunk_OnMeshApplied);
    SCOPE_CYCLE_COUNTER(STAT_Voxel_Chunk_OnApplied);
    CachedBuffers = MoveTemp(AppliedBuffers);

    if (bCancelPending) { State = EVoxelChunkState::Unloading; bIsMeshing = false; return; }

    State = EVoxelChunkState::Ready;
    bIsMeshing = false;

    if (!bWasSeamRemesh && !bHasAnnouncedReady)
    {
        bHasAnnouncedReady = true;
        if (OwnerWorld) OwnerWorld->OnChunkReady(ChunkCoord);
    }

    if (bSeamRemeshQueued)
    {
        bSeamRemeshQueued = false;
        StartMeshing(/*bSeamRemesh=*/true);
    }
}

void UVoxelChunkComponent::OnCollisionReapplied()
{
    if (State != EVoxelChunkState::Unloading)
    {
        State = EVoxelChunkState::Ready;
    }
}

void UVoxelChunkComponent::RequestRemesh()
{
    if (State == EVoxelChunkState::Ready && !bIsMeshing)
    {
        StartMeshing(/*bSeamRemesh=*/true);
    }
    else
    {
        bSeamRemeshQueued = true;
    }
}

void UVoxelChunkComponent::RequestCollisionReapply(bool bNewCollision)
{
    bBuildCollision = bNewCollision;
    if (CachedBuffers.IsValid() && OwnerWorld)
    {
        OwnerWorld->EnqueueReapplyUsingCache(this, bBuildCollision);
    }
    else
    {
        RequestRemesh();
    }
}

void UVoxelChunkComponent::CancelPendingTask()
{
    bCancelPending.AtomicSet(true);
    State = EVoxelChunkState::Unloading;
}

void UVoxelChunkComponent::UnloadChunk()
{
    bCancelPending.AtomicSet(true);
    DestroyMeshComponent();
    VoxelData.Empty();
    HeightData.Empty();
    CachedBuffers.Reset();

    UnregisterComponent();
    DestroyComponent();
}

void UVoxelChunkComponent::SnapshotNeighbors(FChunkNeighbors& Out) const
{
    if (RenderMode != EVoxelRenderMode::Voxels) { Out = {}; return; }

    Out.SizeX = (Settings->ChunkSizeX + LODScaleXY - 1) / LODScaleXY;
    Out.SizeY = (Settings->ChunkSizeY + LODScaleXY - 1) / LODScaleXY;
    Out.SizeZ = Settings->ChunkSizeZ;

    auto CopyXBorder = [&](const TArray<EVoxelBlockID>& Src, int32 SrcSizeX, int32 SrcSizeY, int32 SrcX, TArray<EVoxelBlockID>& Dst, bool& bFlag)
        {
            Dst.SetNumUninitialized(Out.SizeY * Out.SizeZ);
            int32 k = 0;
            for (int32 z = 0; z < Out.SizeZ; ++z)
                for (int32 y = 0; y < Out.SizeY; ++y, ++k)
                {
                    const int32 Index = SrcX + y * SrcSizeX + z * SrcSizeX * SrcSizeY;
                    Dst[k] = Src[Index];
                }
            bFlag = true;
        };

    auto CopyYBorder = [&](const TArray<EVoxelBlockID>& Src, int32 SrcSizeX, int32 SrcSizeY, int32 SrcY, TArray<EVoxelBlockID>& Dst, bool& bFlag)
        {
            Dst.SetNumUninitialized(Out.SizeX * Out.SizeZ);
            int32 k = 0;
            for (int32 z = 0; z < Out.SizeZ; ++z)
                for (int32 x = 0; x < Out.SizeX; ++x, ++k)
                {
                    const int32 Index = x + SrcY * SrcSizeX + z * SrcSizeX * SrcSizeY;
                    Dst[k] = Src[Index];
                }
            bFlag = true;
        };

    const int32 SX = Out.SizeX;
    const int32 SY = Out.SizeY;

    if (AVoxelWorld* W = OwnerWorld)
    {
        auto GetIfSameLOD = [&](int dx, int dy)->UVoxelChunkComponent*
            {
                if (UVoxelChunkComponent* N = W->GetChunk(FVoxelCoord(ChunkCoord.Cx + dx, ChunkCoord.Cy + dy, ChunkCoord.Cz)))
                {
                    if (N->State == EVoxelChunkState::Ready && N->LOD == LOD && N->RenderMode == RenderMode)
                        return N;
                }
                return nullptr;
            };

        if (UVoxelChunkComponent* N = GetIfSameLOD(-1, 0)) { CopyXBorder(N->VoxelData, SX, SY, SX - 1, Out.XNeg, Out.bHasXNeg); }
        if (UVoxelChunkComponent* N = GetIfSameLOD(1, 0)) { CopyXBorder(N->VoxelData, SX, SY, 0, Out.XPos, Out.bHasXPos); }
        if (UVoxelChunkComponent* N = GetIfSameLOD(0, -1)) { CopyYBorder(N->VoxelData, SX, SY, SY - 1, Out.YNeg, Out.bHasYNeg); }
        if (UVoxelChunkComponent* N = GetIfSameLOD(0, 1)) { CopyYBorder(N->VoxelData, SX, SY, 0, Out.YPos, Out.bHasYPos); }
    }
}

void UVoxelChunkComponent::ConvertDenseToCompact(bool bForLOD0)
{
    const int32 XYScaleHere = LODScaleXY;
    const int32 SX = (Settings->ChunkSizeX + XYScaleHere - 1) / XYScaleHere;
    const int32 SY = (Settings->ChunkSizeY + XYScaleHere - 1) / XYScaleHere;
    const int32 SZ = Settings->ChunkSizeZ;
    const int32 N = SX * SY * SZ;

    Compact.SizeX = SX; Compact.SizeY = SY; Compact.SizeZ = SZ;
    Compact.XYScale = XYScaleHere;
    Compact.Occupancy.Reset(); Compact.Occupancy.SetNumZeroed((N + 63) >> 6);
    Compact.Ids.Reset();

    // If we have no dense data (generator-only path), nothing to convert
    if (VoxelData.Num() == 0) { UVoxelMesher::RebuildPrefix64(Compact); return; }

    // Pack occupancy + IDs. Air == 0.
    for (int32 z = 0; z < SZ; ++z)
        for (int32 y = 0; y < SY; ++y)
            for (int32 x = 0; x < SX; ++x)
            {
                const int32 denseX = x * XYScaleHere;
                const int32 denseY = y * XYScaleHere;
                const int32 idxDense = denseX + denseY * Settings->ChunkSizeX + z * Settings->ChunkSizeX * Settings->ChunkSizeY;
                const EVoxelBlockID id = VoxelData.IsValidIndex(idxDense) ? VoxelData[idxDense] : EVoxelBlockID(0);
                if ((uint16)id != 0)
                {
                    const int32 i = UVoxelMesher::Idx3D(x, y, z, SX, SY);
                    Compact.Occupancy[i >> 6] |= (1ULL << (i & 63));
                    Compact.Ids.Add((uint16)id);
                }
            }

    UVoxelMesher::RebuildPrefix64(Compact);

    // Free dense storage for this LOD
    if (bForLOD0 || LOD == EVoxelLODLevel::LOD1)
        VoxelData.Empty();
}

bool UVoxelChunkComponent::SetVoxelCompact(int32 X, int32 Y, int32 Z, uint16 Id)
{
    if ((uint32)X >= (uint32)Compact.SizeX || (uint32)Y >= (uint32)Compact.SizeY || (uint32)Z >= (uint32)Compact.SizeZ) return false;
    const int32 i = UVoxelMesher::Idx3D(X, Y, Z, Compact.SizeX, Compact.SizeY);
    const int32 w = i >> 6, b = i & 63;
    const bool wasSolid = ((Compact.Occupancy[w] >> b) & 1ULL) != 0;

    if (Id == 0) return ClearVoxelCompact(X, Y, Z);

    if (wasSolid)
    {
        // Overwrite ID in-place
        const int32 rank = UVoxelMesher::Rank1_Prefix(Compact, i);
        if ((uint32)rank < (uint32)Compact.Ids.Num())
        {
            const bool changed = (Compact.Ids[rank] != Id);
            Compact.Ids[rank] = Id;
            return changed;
        }
        return false;
    }
    else
    {
        // Insert: set bit and insert ID at position = rank
        const int32 rank = UVoxelMesher::Rank1_Prefix(Compact, i);
        Compact.Occupancy[w] |= (1ULL << b);
        Compact.Ids.Insert(Id, rank);

        // update Prefix64 for all subsequent words
        for (int32 ww = w + 1; ww < Compact.Prefix64.Num(); ++ww) ++Compact.Prefix64[ww];
        return true;
    }
}

bool UVoxelChunkComponent::ClearVoxelCompact(int32 X, int32 Y, int32 Z)
{
    if ((uint32)X >= (uint32)Compact.SizeX || (uint32)Y >= (uint32)Compact.SizeY || (uint32)Z >= (uint32)Compact.SizeZ) return false;
    const int32 i = UVoxelMesher::Idx3D(X, Y, Z, Compact.SizeX, Compact.SizeY);
    const int32 w = i >> 6, b = i & 63;
    const bool wasSolid = ((Compact.Occupancy[w] >> b) & 1ULL) != 0;
    if (!wasSolid) return false;

    const int32 rank = UVoxelMesher::Rank1_Prefix(Compact, i);
    if ((uint32)rank < (uint32)Compact.Ids.Num())
        Compact.Ids.RemoveAt(rank, 1, /*bAllowShrinking*/false);

    Compact.Occupancy[w] &= ~(1ULL << b);
    for (int32 ww = w + 1; ww < Compact.Prefix64.Num(); ++ww) --Compact.Prefix64[ww];
    return true;
}
