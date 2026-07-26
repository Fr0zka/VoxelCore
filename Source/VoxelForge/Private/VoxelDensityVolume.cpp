// VoxelDensityVolume.cpp — see VoxelDensityVolume.h for the design.
// Step 1a: CPU density clipmap + worker fills + toroidal streaming + carve dirty + debug draw.
// The GPU upload (UploadDirtyRegionToGPU) is the step-1b seam and is a no-op here.

#include "VoxelDensityVolume.h"
#include "VoxelGenerator.h"
#include "VoxelSettings.h"
#include "GameFramework/Actor.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Math/UnrealMathUtility.h"
#include "Engine/VolumeTexture.h"
#include "TextureResource.h"
#include "RenderingThread.h"     // ENQUEUE_RENDER_COMMAND
#include "RHICommandList.h"      // FRHICommandListImmediate::UpdateTexture3D
#if ENABLE_DRAW_DEBUG
#include "DrawDebugHelpers.h"
#endif

// Dedicated fill thread: drains FillQueue (Spsc, game thread → here), re-evaluates GetDensityAt via
// the owner's ProcessOneFill, pushes FFillResult into the owner's Results (Mpsc, drained game-side).
// Sleeps on FillWakeEvent when idle. Off the UE::Tasks pool by design — so volume fills run at full
// speed on their own core WITHOUT contending with mesh-gen (the old BackgroundLow path starved).
class FVoxelDensityFillRunnable : public FRunnable
{
public:
    explicit FVoxelDensityFillRunnable(UVoxelDensityVolume* InOwner) : Owner(InOwner) {}

    virtual uint32 Run() override
    {
        while (!Owner->bFillThreadStop.load(std::memory_order_acquire))
        {
            UVoxelDensityVolume::FPendingFill F;
            bool bDidWork = false;
            while (Owner->FillQueue.Dequeue(F))
            {
                bDidWork = true;
                Owner->ProcessOneFill(F);
                if (Owner->bFillThreadStop.load(std::memory_order_relaxed)) break;
            }
            // Sleep until the game thread queues more (or asks us to stop). The Trigger() always
            // follows the Enqueue(), so a trigger landing here is latched by the auto-reset event →
            // no missed wakeup.
            if (!bDidWork && Owner->FillWakeEvent)
            {
                Owner->FillWakeEvent->Wait();
            }
        }
        return 0;
    }

    virtual void Stop() override
    {
        Owner->bFillThreadStop.store(true, std::memory_order_release);
        if (Owner->FillWakeEvent) { Owner->FillWakeEvent->Trigger(); }
    }

private:
    UVoxelDensityVolume* Owner;
};

//=============================================================================
// Lifecycle
//=============================================================================

void UVoxelDensityVolume::Initialize(AActor* InOwner, UVoxelGenerator* InGenerator, UVoxelSettings* InSettings)
{
    Owner = InOwner;
    Generator = InGenerator;
    Settings = InSettings;
    bShuttingDown.store(false, std::memory_order_relaxed);
    bInitialized = true;
    // Arrays are allocated lazily on the first Update (EnsureAllocated) so a settings change
    // (resolution / level count) before play picks up cleanly.
}

void UVoxelDensityVolume::BeginDestroy()
{
    // Backstop — EndPlay → NotifyShutdown should already have stopped the fill thread.
    bShuttingDown.store(true, std::memory_order_release);
    StopFillThread();
    Super::BeginDestroy();
}

void UVoxelDensityVolume::NotifyShutdown()
{
    bShuttingDown.store(true, std::memory_order_release);

    // Stop the dedicated fill thread — Kill(true) blocks until Run() returns, so it can't read the
    // Generator after this (the owner tears UObjects down next). Then drop any queued/finished work.
    StopFillThread();

    FFillResult Discard;
    while (Results.Dequeue(Discard)) {}
    PendingFills.Reset();
    CaptureCache.Empty();
}

void UVoxelDensityVolume::Reset()
{
    // Bump the epoch so any in-flight fill lands stale and is dropped in DrainResults.
    ++VolumeEpoch;
    PendingFills.Reset();
    FFillResult Discard;
    while (Results.Dequeue(Discard)) {}

    // Drop all data → next Update full-refills every level (origin sentinel + bHasData false).
    for (FClipLevel& Lv : Levels)
    {
        Lv.OriginCells = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
        Lv.bHasData = false;
        Lv.bGPUDirty = true;   // upload the cleared (zero) data; the refill then re-uploads real data
        if (Lv.Density.Num() > 0) { FMemory::Memzero(Lv.Density.GetData(), Lv.Density.Num()); }
    }
    CaptureCache.Empty();   // pre-reset grids belong to the old world (epoch bumped)
    LastPlayerVoxel = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
}

//=============================================================================
// Helpers
//=============================================================================

int32 UVoxelDensityVolume::ResPerAxis() const
{
    return FMath::Clamp(Settings ? Settings->DensityVolumeResolution : 128, 32, 256);
}

int32 UVoxelDensityVolume::NumLevels() const
{
    return FMath::Clamp(Settings ? Settings->DensityVolumeLevels : 3, 1, 5);
}

FORCEINLINE int32 UVoxelDensityVolume::FloorDiv(int32 A, int32 B)
{
    // True floor division (B > 0). FMath::DivideAndRoundDown truncates toward zero for negatives —
    // a footgun the content manager hit too (see FloorDivPos there). Cells span the origin, so floor.
    return (A >= 0) ? (A / B) : -(((-A) + B - 1) / B);
}

FORCEINLINE uint8 UVoxelDensityVolume::Quantize(float MCDensity)
{
    // Single source of truth (VoxelTypes.h) — MUST stay bit-identical with the mesher's
    // capture-during-meshing path (UVoxelMarchingCubesMesher::GenerateMesh OutCaptureGrid),
    // so an ingested tile capture equals a worker fill of the same cells byte-for-byte.
    return VF_QuantizeDensity(MCDensity);
}

void UVoxelDensityVolume::EnsureAllocated()
{
    const int32 Res = ResPerAxis();
    const int32 N = NumLevels();
    if (AllocatedRes == Res && Levels.Num() == N) return;   // already sized

    Levels.Reset();
    Levels.SetNum(N);
    const int32 Count = Res * Res * Res;
    for (int32 L = 0; L < N; ++L)
    {
        FClipLevel& Lv = Levels[L];
        Lv.Step = 1 << L;
        Lv.OriginCells = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
        Lv.bHasData = false;
        Lv.Density.SetNumZeroed(Count);   // start all-air (0)
    }
    AllocatedRes = Res;
    LastPlayerVoxel = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);   // force a recenter

    EnsureTextures();
}

//=============================================================================
// GPU upload (step 1b-i) — per-level R8 volume textures + full re-upload of dirty levels
//=============================================================================

void UVoxelDensityVolume::EnsureTextures()
{
    if (!Settings || !Settings->bEnableDensityVolume || !Settings->bDensityVolumeGPUUpload) return;
    const int32 Res = ResPerAxis();
    const int32 N = NumLevels();
    if (LevelTextures.Num() == N && AllocatedTexRes == Res) return;   // already sized

    for (TObjectPtr<UVolumeTexture>& T : LevelTextures)
    {
        if (T) { T->ReleaseResource(); }
    }
    LevelTextures.Reset();
    LevelTextures.SetNum(N);

    const int64 Count = (int64)Res * Res * Res;
    for (int32 L = 0; L < N; ++L)
    {
        UVolumeTexture* T = NewObject<UVolumeTexture>(this);
        T->SRGB = false;
        T->Filter = TF_Trilinear;                 // smooth iso crossing (sub-voxel crisp edge)
        T->CompressionSettings = TC_Grayscale;    // single-channel
        T->NeverStream = true;
#if WITH_EDITORONLY_DATA
        // MipGenSettings n'existe que dans les builds éditeur (WITH_EDITORONLY_DATA) — c'est un hint
        // pour le mip-builder du cooker. En packagé le PlatformData construit ici n'a qu'UN mip de
        // toute façon (1b-i: base mip only; solidity mips come with the march).
        T->MipGenSettings = TMGS_NoMipmaps;
#endif

        // Runtime platform data: one R8 (PF_G8) mip, zero-initialised. NOTE (UE5.7 API surface — flag if
        // the build rejects any of these): FTexturePlatformData / SetNumSlices / SetPlatformData /
        // FTexture2DMipMap(with SizeZ for volumes). If the names drifted, this whole GPU path is gated by
        // bDensityVolumeGPUUpload — turn it off to fall back to the validated CPU volume while we fix it.
        FTexturePlatformData* PD = new FTexturePlatformData();
        PD->SizeX = Res;
        PD->SizeY = Res;
        PD->PixelFormat = PF_G8;
        PD->SetNumSlices(Res);

        FTexture2DMipMap* Mip = new FTexture2DMipMap();
        Mip->SizeX = Res;
        Mip->SizeY = Res;
        Mip->SizeZ = Res;
        Mip->BulkData.Lock(LOCK_READ_WRITE);
        void* Dst = Mip->BulkData.Realloc(Count);
        FMemory::Memzero(Dst, Count);
        Mip->BulkData.Unlock();
        PD->Mips.Add(Mip);

        T->SetPlatformData(PD);
        T->UpdateResource();
        LevelTextures[L] = T;
    }
    AllocatedTexRes = Res;

    // New textures are zeroed → mark every level dirty so the current CPU data uploads.
    for (FClipLevel& Lv : Levels) { Lv.bGPUDirty = true; }
}

void UVoxelDensityVolume::UploadDirtyTextures()
{
    if (!Settings || !Settings->bDensityVolumeGPUUpload) return;
    EnsureTextures();   // cheap early-out when sized; covers bDensityVolumeGPUUpload toggled ON at runtime
    const int32 Res = ResPerAxis();
    for (int32 L = 0; L < Levels.Num(); ++L)
    {
        FClipLevel& Lv = Levels[L];
        if (!Lv.bGPUDirty) continue;
        if (!LevelTextures.IsValidIndex(L) || !LevelTextures[L]) continue;
        FTextureResource* Resource = LevelTextures[L]->GetResource();
        if (!Resource) continue;
        Lv.bGPUDirty = false;

        // The CPU toroidal array IS the texture's memory layout (texel (tx,ty,tz) = array[(tz*Res+ty)*Res+tx]),
        // so a FULL re-upload from it is correct with no wrap-splitting. ~Res³ bytes/level (2 MB at 128) — only
        // when the level actually changed (idle = no upload). Sub-box upload (with toroidal wrap-splitting) is
        // a later optimisation. Copy the source for the render thread (the CPU array keeps mutating).
        TArray<uint8> Src = Lv.Density;
        ENQUEUE_RENDER_COMMAND(VoxelDensityVolumeUpload)(
            [Resource, SrcData = MoveTemp(Src), Res](FRHICommandListImmediate& RHICmdList) mutable
            {
                FRHITexture* Tex = Resource->GetTextureRHI();
                if (!Tex) return;
                const FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0, Res, Res, Res);
                // R8: row pitch = Res bytes, depth (slice) pitch = Res*Res bytes.
                RHICmdList.UpdateTexture3D(Tex, 0, Region, (uint32)Res, (uint32)(Res * Res), SrcData.GetData());
            });
    }
}

UVolumeTexture* UVoxelDensityVolume::GetLevelTexture(int32 Level) const
{
    return LevelTextures.IsValidIndex(Level) ? LevelTextures[Level].Get() : nullptr;
}

bool UVoxelDensityVolume::GetLevelShaderParams(int32 Level, FIntVector& OutOriginCells, float& OutStep, int32& OutRes) const
{
    if (!Levels.IsValidIndex(Level) || !Levels[Level].bHasData) return false;
    OutOriginCells = Levels[Level].OriginCells;
    OutStep = (float)Levels[Level].Step;
    OutRes = ResPerAxis();
    return true;
}

//=============================================================================
// Update — recentre, queue, launch, drain
//=============================================================================

void UVoxelDensityVolume::Update(const FVector& PlayerWorldPos)
{
    if (!bInitialized || !Settings || !Settings->bEnableDensityVolume || !Generator) return;
    AActor* O = Owner.Get();
    if (!O) return;

    EnsureAllocated();

    // Player world → actor-local voxel coords (GetDensityAt is in actor-local voxel space, same as the
    // decoration march). The actor is Static at the origin, but do it properly via the transform.
    const FTransform Xf = O->GetActorTransform();
    const FVector Local = Xf.InverseTransformPosition(PlayerWorldPos);
    const FIntVector PlayerVoxel(
        FMath::RoundToInt(Local.X / VOXEL_SIZE),
        FMath::RoundToInt(Local.Y / VOXEL_SIZE),
        FMath::RoundToInt(Local.Z / VOXEL_SIZE));

    if (PlayerVoxel != LastPlayerVoxel)
    {
        LastPlayerVoxel = PlayerVoxel;
        const int32 N = Levels.Num();
        for (int32 L = 0; L < N; ++L)
        {
            RecenterLevel(L, PlayerVoxel);   // cheap no-op for a level whose origin didn't move
        }
    }

    LaunchPendingFills();   // drain the queue under the task budget
    DrainResults();         // apply finished worker fills into the toroidal arrays (marks levels GPU-dirty)
    UploadDirtyTextures();  // push changed levels to the GPU (render-thread RHIUpdateTexture3D)
}

void UVoxelDensityVolume::RecenterLevel(int32 L, const FIntVector& PlayerVoxel)
{
    if (!Levels.IsValidIndex(L)) return;
    FClipLevel& Lv = Levels[L];
    const int32 Res = ResPerAxis();
    const int32 Step = Lv.Step;
    const int32 Half = Res / 2;

    const FIntVector PlayerCell(FloorDiv(PlayerVoxel.X, Step),
                                FloorDiv(PlayerVoxel.Y, Step),
                                FloorDiv(PlayerVoxel.Z, Step));
    FIntVector NewOrigin = PlayerCell - FIntVector(Half, Half, Half);

    // LEVEL 0 is capture-fed (capture-during-meshing). Snap the window origin to the level-0 TILE grid
    // (CHUNK_SIZE cells) so newly-exposed slabs align to whole captured tiles, and the window only
    // scrolls on CHUNK crossings (≈CHUNK_SIZE× fewer recenters + GPU re-uploads than the per-voxel
    // path). The player still stays ≥CHUNK_SIZE cells from any window edge, so the small origin offset
    // is invisible to the shadow march. Coarser levels keep the per-cell worker-fill path unchanged.
    const bool bCapture = (L == 0);
    if (bCapture)
    {
        NewOrigin = FIntVector(FloorDiv(NewOrigin.X, CHUNK_SIZE) * CHUNK_SIZE,
                               FloorDiv(NewOrigin.Y, CHUNK_SIZE) * CHUNK_SIZE,
                               FloorDiv(NewOrigin.Z, CHUNK_SIZE) * CHUNK_SIZE);
    }
    const FIntVector Dim(Res, Res, Res);

    if (Lv.bHasData && NewOrigin == Lv.OriginCells) return;   // didn't move → nothing to refill

    if (!Lv.bHasData)
    {
        Lv.OriginCells = NewOrigin;
        Lv.bHasData = true;                 // toroidal slots are stale until the fills land (transient)
        if (bCapture) { FillBoxFromCacheOrQueue(NewOrigin, Dim); EvictFarCaptures(); }
        else          { QueueFillSplit(L, NewOrigin, Dim); }
        return;
    }

    // Incremental: refill only the slabs that scrolled into view (new window minus old window). The
    // toroidal slots for cells still in view keep their valid data — no copy/move needed.
    TArray<TPair<FIntVector, FIntVector>> Boxes;
    BoxDifference(NewOrigin, Dim, Lv.OriginCells, Dim, Boxes);
    Lv.OriginCells = NewOrigin;
    for (const TPair<FIntVector, FIntVector>& B : Boxes)
    {
        if (bCapture) FillBoxFromCacheOrQueue(B.Key, B.Value);
        else          QueueFillSplit(L, B.Key, B.Value);
    }
    if (bCapture) EvictFarCaptures();
}

void UVoxelDensityVolume::BoxDifference(const FIntVector& NewMin, const FIntVector& NewDim,
                                        const FIntVector& OldMin, const FIntVector& OldDim,
                                        TArray<TPair<FIntVector, FIntVector>>& OutBoxes)
{
    const FIntVector NMax = NewMin + NewDim;   // exclusive
    const FIntVector OMax = OldMin + OldDim;
    const FIntVector IMin(FMath::Max(NewMin.X, OldMin.X), FMath::Max(NewMin.Y, OldMin.Y), FMath::Max(NewMin.Z, OldMin.Z));
    const FIntVector IMax(FMath::Min(NMax.X, OMax.X),     FMath::Min(NMax.Y, OMax.Y),     FMath::Min(NMax.Z, OMax.Z));

    // No overlap → the whole new window is new.
    if (IMin.X >= IMax.X || IMin.Y >= IMax.Y || IMin.Z >= IMax.Z)
    {
        OutBoxes.Add(TPair<FIntVector, FIntVector>(NewMin, NewDim));
        return;
    }

    auto Add = [&](int32 x0, int32 x1, int32 y0, int32 y1, int32 z0, int32 z1)
    {
        if (x1 > x0 && y1 > y0 && z1 > z0)
        {
            OutBoxes.Add(TPair<FIntVector, FIntVector>(FIntVector(x0, y0, z0), FIntVector(x1 - x0, y1 - y0, z1 - z0)));
        }
    };

    // X slabs span the full new Y,Z; Y slabs span the overlap X + full new Z; Z slabs span the overlap X,Y.
    // Together these are disjoint and cover (new \ old) exactly.
    Add(NewMin.X, IMin.X, NewMin.Y, NMax.Y, NewMin.Z, NMax.Z);
    Add(IMax.X,   NMax.X, NewMin.Y, NMax.Y, NewMin.Z, NMax.Z);
    Add(IMin.X, IMax.X, NewMin.Y, IMin.Y, NewMin.Z, NMax.Z);
    Add(IMin.X, IMax.X, IMax.Y,   NMax.Y, NewMin.Z, NMax.Z);
    Add(IMin.X, IMax.X, IMin.Y, IMax.Y, NewMin.Z, IMin.Z);
    Add(IMin.X, IMax.X, IMin.Y, IMax.Y, IMax.Z,   NMax.Z);
}

void UVoxelDensityVolume::QueueFillSplit(int32 L, const FIntVector& MinCells, const FIntVector& DimCells)
{
    if (DimCells.X <= 0 || DimCells.Y <= 0 || DimCells.Z <= 0) return;
    const int32 Slab = FMath::Clamp(Settings ? Settings->DensityVolumeFillSlabCells : 8, 1, 64);
    for (int32 z0 = 0; z0 < DimCells.Z; z0 += Slab)
    {
        const int32 dz = FMath::Min(Slab, DimCells.Z - z0);
        FPendingFill F;
        F.Level = L;
        F.MinCells = FIntVector(MinCells.X, MinCells.Y, MinCells.Z + z0);
        F.DimCells = FIntVector(DimCells.X, DimCells.Y, dz);
        F.Epoch = VolumeEpoch;
        PendingFills.Add(MoveTemp(F));
    }
}

//=============================================================================
// Capture-during-meshing (level-0): cache-fed fills, ingest, eviction
//=============================================================================

void UVoxelDensityVolume::FillBoxFromCacheOrQueue(const FIntVector& MinCells, const FIntVector& DimCells)
{
    if (DimCells.X <= 0 || DimCells.Y <= 0 || DimCells.Z <= 0) return;
    const FIntVector BoxMax = MinCells + DimCells;   // exclusive (level 0: cell coord == voxel coord)

    // Iterate the level-0 tiles overlapping the box (a tile spans CHUNK_SIZE cells). Cached tiles blit
    // straight from the captured grid (no GetDensityAt); uncached tiles fall back to a worker fill of
    // just the box∩tile region (cold start, vertical strate gaps, evicted tiles).
    const FIntVector TMin(FloorDiv(MinCells.X, CHUNK_SIZE), FloorDiv(MinCells.Y, CHUNK_SIZE), FloorDiv(MinCells.Z, CHUNK_SIZE));
    const FIntVector TMax(FloorDiv(BoxMax.X - 1, CHUNK_SIZE), FloorDiv(BoxMax.Y - 1, CHUNK_SIZE), FloorDiv(BoxMax.Z - 1, CHUNK_SIZE));

    for (int32 tz = TMin.Z; tz <= TMax.Z; ++tz)
    for (int32 ty = TMin.Y; ty <= TMax.Y; ++ty)
    for (int32 tx = TMin.X; tx <= TMax.X; ++tx)
    {
        const FIntVector T(tx, ty, tz);
        if (const TArray<uint8>* Grid = CaptureCache.Find(T))
        {
            BlitCaptureToWindow(T, *Grid);   // writes all of T's in-window cells (idempotent)
        }
        else
        {
            const FIntVector Org = T * CHUNK_SIZE;
            const FIntVector IMin(FMath::Max(MinCells.X, Org.X), FMath::Max(MinCells.Y, Org.Y), FMath::Max(MinCells.Z, Org.Z));
            const FIntVector IMax(FMath::Min(BoxMax.X, Org.X + CHUNK_SIZE),
                                  FMath::Min(BoxMax.Y, Org.Y + CHUNK_SIZE),
                                  FMath::Min(BoxMax.Z, Org.Z + CHUNK_SIZE));   // exclusive
            QueueFillSplit(0, IMin, FIntVector(IMax.X - IMin.X, IMax.Y - IMin.Y, IMax.Z - IMin.Z));
        }
    }
}

bool UVoxelDensityVolume::BlitCaptureToWindow(const FIntVector& L0TileCoord, const TArray<uint8>& Grid)
{
    if (!Levels.IsValidIndex(0)) return false;
    FClipLevel& Lv = Levels[0];
    if (!Lv.bHasData) return false;
    if (Grid.Num() < CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE) return false;

    const int32 Res = ResPerAxis();
    const FIntVector W0 = Lv.OriginCells;
    const FIntVector W1 = Lv.OriginCells + FIntVector(Res, Res, Res);   // exclusive
    const FIntVector Org = L0TileCoord * CHUNK_SIZE;                    // tile min cell == min voxel (step 1)

    // Clamp the tile to the window once (per axis); reject if fully outside.
    const int32 cx0 = FMath::Max(W0.X, Org.X), cx1 = FMath::Min(W1.X, Org.X + CHUNK_SIZE);
    const int32 cy0 = FMath::Max(W0.Y, Org.Y), cy1 = FMath::Min(W1.Y, Org.Y + CHUNK_SIZE);
    const int32 cz0 = FMath::Max(W0.Z, Org.Z), cz1 = FMath::Min(W1.Z, Org.Z + CHUNK_SIZE);
    if (cx0 >= cx1 || cy0 >= cy1 || cz0 >= cz1) return false;

    // Toroidal walk: one modulo per ROW, then the X run increments tx and wraps manually (this is on
    // the game thread and batches a whole tile per chunk crossing — per-cell modulo would spike).
    uint8* RESTRICT Dst = Lv.Density.GetData();
    const uint8* RESTRICT Src = Grid.GetData();
    const int32 tx0 = ((cx0 % Res) + Res) % Res;
    const int32 gx0 = cx0 - Org.X;
    for (int32 cz = cz0; cz < cz1; ++cz)
    {
        const int32 tz = ((cz % Res) + Res) % Res;
        const int32 gz = cz - Org.Z;
        for (int32 cy = cy0; cy < cy1; ++cy)
        {
            const int32 ty = ((cy % Res) + Res) % Res;
            const int32 DstRow = (tz * Res + ty) * Res;
            const int32 SrcRow = (gz * CHUNK_SIZE + (cy - Org.Y)) * CHUNK_SIZE;
            int32 tx = tx0, gx = gx0;
            for (int32 cx = cx0; cx < cx1; ++cx)
            {
                Dst[DstRow + tx] = Src[SrcRow + gx];
                ++gx;
                if (++tx == Res) tx = 0;
            }
        }
    }
    Lv.bGPUDirty = true;
    return true;
}

bool UVoxelDensityVolume::GetCaptureKeepBounds(FIntVector& OutLo, FIntVector& OutHi) const
{
    if (!Levels.IsValidIndex(0) || !Levels[0].bHasData) return false;
    const int32 Res = ResPerAxis();
    const FIntVector W0 = Levels[0].OriginCells;

    // Tiles overlapping the window, +1 tile margin (keep the lead shell so a just-loaded tile isn't
    // dropped before the window scrolls onto it).
    OutLo = FIntVector(FloorDiv(W0.X, CHUNK_SIZE) - 1, FloorDiv(W0.Y, CHUNK_SIZE) - 1, FloorDiv(W0.Z, CHUNK_SIZE) - 1);
    OutHi = FIntVector(FloorDiv(W0.X + Res - 1, CHUNK_SIZE) + 1, FloorDiv(W0.Y + Res - 1, CHUNK_SIZE) + 1, FloorDiv(W0.Z + Res - 1, CHUNK_SIZE) + 1);
    return true;
}

void UVoxelDensityVolume::EvictFarCaptures()
{
    if (CaptureCache.Num() == 0) return;
    FIntVector TLo, THi;
    if (!GetCaptureKeepBounds(TLo, THi)) return;
    for (auto It = CaptureCache.CreateIterator(); It; ++It)
    {
        const FIntVector& T = It.Key();
        if (T.X < TLo.X || T.X > THi.X || T.Y < TLo.Y || T.Y > THi.Y || T.Z < TLo.Z || T.Z > THi.Z)
        {
            It.RemoveCurrent();
        }
    }
}

bool UVoxelDensityVolume::IsTileCaptureUseful(const FIntVector& L0TileCoord) const
{
    FIntVector TLo, THi;
    if (!GetCaptureKeepBounds(TLo, THi)) return true;   // no window yet → keep (cold start)
    return L0TileCoord.X >= TLo.X && L0TileCoord.X <= THi.X
        && L0TileCoord.Y >= TLo.Y && L0TileCoord.Y <= THi.Y
        && L0TileCoord.Z >= TLo.Z && L0TileCoord.Z <= THi.Z;
}

void UVoxelDensityVolume::IngestTileCapture(const FIntVector& L0TileCoord, TArray<uint8>&& Grid)
{
    if (!bInitialized || !Settings || !Settings->bEnableDensityVolume) return;
    if (Grid.Num() < CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE) return;

    // Only cache tiles inside the keep bounds (window + lead-shell margin). The level-0 STREAMING ring
    // is much larger than the shadow window — most streamed tiles can never blit and would only sit in
    // the cache (32 KB each) until the next recenter evicted them. Same policy EvictFarCaptures applies.
    // (LoadTile already pre-gates the capture with this test; this re-check is authoritative in case
    // the window scrolled while the tile's gen task was in flight.)
    if (!IsTileCaptureUseful(L0TileCoord)) return;

    // Store (overwrite) — the cache is RecenterLevel(0)'s fill source and survives until the tile
    // scrolls out of the window. Blit now so cells already in view refresh immediately (a tile that
    // finished after the window exposed it, or a re-mesh after a carve).
    TArray<uint8>& Slot = CaptureCache.FindOrAdd(L0TileCoord);
    Slot = MoveTemp(Grid);
    BlitCaptureToWindow(L0TileCoord, Slot);
}

void UVoxelDensityVolume::EnsureFillThread()
{
    if (FillThread) return;
    if (!Settings || !Settings->bEnableDensityVolume) return;
    bFillThreadStop.store(false, std::memory_order_release);
    if (!FillWakeEvent) { FillWakeEvent = FPlatformProcess::GetSynchEventFromPool(false); }   // auto-reset
    FillRunnable = new FVoxelDensityFillRunnable(this);
    FillThread = FRunnableThread::Create(FillRunnable, TEXT("VoxelDensityFill"), 0, TPri_Normal);
    if (!FillThread)   // creation failed → don't leak the runnable; fills just won't drain (no crash)
    {
        delete FillRunnable;
        FillRunnable = nullptr;
    }
}

void UVoxelDensityVolume::StopFillThread()
{
    bFillThreadStop.store(true, std::memory_order_release);
    if (FillWakeEvent) { FillWakeEvent->Trigger(); }   // wake it so it sees the stop
    if (FillThread)
    {
        FillThread->Kill(true);   // calls Stop() + blocks until Run() returns (no more Generator reads)
        delete FillThread;
        FillThread = nullptr;
    }
    if (FillRunnable) { delete FillRunnable; FillRunnable = nullptr; }
    if (FillWakeEvent) { FPlatformProcess::ReturnSynchEventToPool(FillWakeEvent); FillWakeEvent = nullptr; }
    FPendingFill Discard;
    while (FillQueue.Dequeue(Discard)) {}
}

void UVoxelDensityVolume::LaunchPendingFills()
{
    if (PendingFills.Num() == 0) return;
    EnsureFillThread();
    for (FPendingFill& F : PendingFills)
    {
        FillQueue.Enqueue(MoveTemp(F));   // Spsc: game thread is the only producer
    }
    PendingFills.Reset();
    if (FillWakeEvent) { FillWakeEvent->Trigger(); }   // wake the fill thread (after the Enqueues)
}

// RUNS ON THE FILL THREAD. Reads only the Generator (thread-safe, deterministic — same contract as the
// old worker tasks) and pushes the result into the Mpsc Results queue. Step = 1<<Level (never read from
// the game-thread-mutated Levels array). Bails on shutdown so the Generator can be torn down after Kill.
void UVoxelDensityVolume::ProcessOneFill(const FPendingFill& F)
{
    UVoxelGenerator* Gen = Generator;
    if (!Gen) return;
    if (bShuttingDown.load(std::memory_order_relaxed) || bFillThreadStop.load(std::memory_order_relaxed)) return;

    const int32 Step = 1 << F.Level;
    FFillResult R;
    R.Level = F.Level;
    R.Epoch = F.Epoch;
    R.MinCells = F.MinCells;
    R.DimCells = F.DimCells;
    const int32 Count = F.DimCells.X * F.DimCells.Y * F.DimCells.Z;
    if (Count <= 0) return;
    R.Data.SetNumUninitialized(Count);

    int32 i = 0;
    for (int32 z = 0; z < F.DimCells.Z; ++z)
    {
        if (bFillThreadStop.load(std::memory_order_relaxed)) return;   // periodic bail on big boxes
        const float WZ = (float)((F.MinCells.Z + z) * Step);
        for (int32 y = 0; y < F.DimCells.Y; ++y)
        {
            const float WY = (float)((F.MinCells.Y + y) * Step);
            for (int32 x = 0; x < F.DimCells.X; ++x)
            {
                const float WX = (float)((F.MinCells.X + x) * Step);
                R.Data[i++] = Quantize(Gen->GetDensityAt(WX, WY, WZ));
            }
        }
    }

    if (bFillThreadStop.load(std::memory_order_relaxed)) return;
    Results.Enqueue(MoveTemp(R));
}

void UVoxelDensityVolume::DrainResults()
{
    const int32 Res = ResPerAxis();
    FFillResult R;
    while (Results.Dequeue(R))
    {
        if (R.Epoch != VolumeEpoch) continue;            // stale (regen/season reset) → drop
        if (!Levels.IsValidIndex(R.Level)) continue;
        FClipLevel& Lv = Levels[R.Level];
        if (!Lv.bHasData) continue;

        const FIntVector W0 = Lv.OriginCells;
        const FIntVector W1 = Lv.OriginCells + FIntVector(Res, Res, Res);   // exclusive

        int32 i = 0;
        for (int32 z = 0; z < R.DimCells.Z; ++z)
        {
            const int32 cz = R.MinCells.Z + z;
            for (int32 y = 0; y < R.DimCells.Y; ++y)
            {
                const int32 cy = R.MinCells.Y + y;
                for (int32 x = 0; x < R.DimCells.X; ++x)
                {
                    const int32 cx = R.MinCells.X + x;
                    const uint8 v = R.Data[i++];
                    // Skip cells that scrolled out of the window since launch — their toroidal slot now
                    // belongs to a different cell (which has its own pending fill). In-window cells own
                    // their slot, so writing is always correct. (A pre-carve fill landing after the
                    // carve's own re-fill is a rare 1-frame transient — both sample GetDensityAt incl.
                    // the diff, so it self-heals on the next refill of that cell.)
                    if (cx < W0.X || cx >= W1.X || cy < W0.Y || cy >= W1.Y || cz < W0.Z || cz >= W1.Z) continue;
                    const int32 tx = ((cx % Res) + Res) % Res;
                    const int32 ty = ((cy % Res) + Res) % Res;
                    const int32 tz = ((cz % Res) + Res) % Res;
                    Lv.Density[(tz * Res + ty) * Res + tx] = v;
                }
            }
        }

        Lv.bGPUDirty = true;   // a fill landed → re-upload this level to the GPU next UploadDirtyTextures
    }
}

//=============================================================================
// Carve invalidation
//=============================================================================

void UVoxelDensityVolume::MarkDirtyVoxelBox(const FIntVector& MinVoxel, const FIntVector& MaxVoxel)
{
    if (!bInitialized || !Settings || !Settings->bEnableDensityVolume) return;
    const int32 Res = ResPerAxis();
    const int32 N = Levels.Num();

    // Capture invalidation (level 0): the cached grids hold PRE-carve density. Drop the ones the carve
    // touched so a later RecenterLevel(0) can't blit stale rock over the edit. The worker fill queued
    // below is the backstop until RemeshDirtyChunks re-meshes the tile and re-ingests a fresh (post-
    // diff) capture. ±1 tile margin to match the carve-falloff bleed used for the cell box below.
    if (CaptureCache.Num() > 0)
    {
        const FIntVector TLo(FloorDiv(MinVoxel.X, CHUNK_SIZE) - 1, FloorDiv(MinVoxel.Y, CHUNK_SIZE) - 1, FloorDiv(MinVoxel.Z, CHUNK_SIZE) - 1);
        const FIntVector THi(FloorDiv(MaxVoxel.X, CHUNK_SIZE) + 1, FloorDiv(MaxVoxel.Y, CHUNK_SIZE) + 1, FloorDiv(MaxVoxel.Z, CHUNK_SIZE) + 1);
        for (int32 tz = TLo.Z; tz <= THi.Z; ++tz)
        for (int32 ty = TLo.Y; ty <= THi.Y; ++ty)
        for (int32 tx = TLo.X; tx <= THi.X; ++tx)
        {
            CaptureCache.Remove(FIntVector(tx, ty, tz));
        }
    }
    for (int32 L = 0; L < N; ++L)
    {
        FClipLevel& Lv = Levels[L];
        if (!Lv.bHasData) continue;
        const int32 Step = Lv.Step;

        // Voxel box → cell box, with a ±1 cell margin (carve falloff bleeds past the exact box).
        FIntVector CMin(FloorDiv(MinVoxel.X, Step) - 1, FloorDiv(MinVoxel.Y, Step) - 1, FloorDiv(MinVoxel.Z, Step) - 1);
        FIntVector CMax(FloorDiv(MaxVoxel.X, Step) + 1, FloorDiv(MaxVoxel.Y, Step) + 1, FloorDiv(MaxVoxel.Z, Step) + 1); // inclusive

        // Clip to the level's current window [Origin, Origin+Res).
        const FIntVector W0 = Lv.OriginCells;
        const FIntVector W1 = Lv.OriginCells + FIntVector(Res, Res, Res);   // exclusive
        CMin = FIntVector(FMath::Max(CMin.X, W0.X), FMath::Max(CMin.Y, W0.Y), FMath::Max(CMin.Z, W0.Z));
        CMax = FIntVector(FMath::Min(CMax.X, W1.X - 1), FMath::Min(CMax.Y, W1.Y - 1), FMath::Min(CMax.Z, W1.Z - 1));
        if (CMax.X < CMin.X || CMax.Y < CMin.Y || CMax.Z < CMin.Z) continue;   // no overlap with this level

        QueueFillSplit(L, CMin, FIntVector(CMax.X - CMin.X + 1, CMax.Y - CMin.Y + 1, CMax.Z - CMin.Z + 1));
    }
}

//=============================================================================
// Debug visualization (step 1a verification — no GPU)
//=============================================================================

#if ENABLE_DRAW_DEBUG
void UVoxelDensityVolume::DebugDraw() const
{
    if (!Settings || !Settings->bDebugDrawDensityVolume) return;
    AActor* O = Owner.Get();
    if (!O || Levels.Num() == 0) return;
    const FClipLevel& Lv = Levels[0];      // level 0 = step 1 → cell coord == voxel coord
    if (!Lv.bHasData) return;
    UWorld* W = O->GetWorld();
    if (!W) return;

    const int32 Res = ResPerAxis();
    const FTransform Xf = O->GetActorTransform();
    const int32 R = FMath::Clamp(Settings->DensityVolumeDebugRadiusCells, 1, 32);
    // Draw a THIN horizontal slab through the player (not a full 3D ball) — far cheaper and it reads
    // as the cave silhouette around you. A full sphere of DrawDebugBox is thousands of boxes/frame =
    // tens of thousands of line segments → big game-thread lag. The volume itself is off-thread.
    const int32 ZBand = 2;   // ±2 cells (5 layers) around the player
    const FIntVector PC = LastPlayerVoxel;                 // level-0 cell == voxel
    const FIntVector W0 = Lv.OriginCells;
    const FIntVector W1 = Lv.OriginCells + FIntVector(Res, Res, Res);
    const float Half = VOXEL_SIZE * 0.5f;

    int32 Drawn = 0;
    const int32 Cap = 4000;   // bound the debug-draw cost
    for (int32 dz = -ZBand; dz <= ZBand; ++dz)
    for (int32 dy = -R; dy <= R; ++dy)
    for (int32 dx = -R; dx <= R; ++dx)
    {
        const int32 cx = PC.X + dx, cy = PC.Y + dy, cz = PC.Z + dz;
        if (cx < W0.X || cx >= W1.X || cy < W0.Y || cy >= W1.Y || cz < W0.Z || cz >= W1.Z) continue;
        const int32 tx = ((cx % Res) + Res) % Res;
        const int32 ty = ((cy % Res) + Res) % Res;
        const int32 tz = ((cz % Res) + Res) % Res;
        if (Lv.Density[(tz * Res + ty) * Res + tx] <= 128) continue;   // air (iso ≈ 128)

        const FVector LocalCenter((cx + 0.5f) * VOXEL_SIZE, (cy + 0.5f) * VOXEL_SIZE, (cz + 0.5f) * VOXEL_SIZE);
        const FVector WorldC = Xf.TransformPosition(LocalCenter);
        DrawDebugBox(W, WorldC, FVector(Half), FColor::Cyan, false, -1.0f, 0, 1.0f);
        if (++Drawn >= Cap) return;
    }
}
#endif
