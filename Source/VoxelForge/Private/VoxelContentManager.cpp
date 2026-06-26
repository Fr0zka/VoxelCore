// VoxelContentManager.cpp
// Distance-based world-grid decoration scatter with ASYNC surface marching (no LOD pop, no frame cost)
// + aesthetic water surfaces.

#include "VoxelContentManager.h"
#include "VoxelStrateManager.h"
#include "VoxelStrateDefinition.h"
#include "VoxelBiomeDefinition.h"
#include "VoxelGenerator.h"
#include "VoxelSettings.h"
#include "VoxelCaveMorphology.h"   // VoxelHash
#include "Components/StaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Tasks/Task.h"
#include "HAL/PlatformProcess.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"   // Unreal Insights scopes

// Global safety cap on spawned decoration ACTORS per cell (across all entries).
// HISM instances are exempt — they're batched render data, capped per entry by MaxPerChunk.
static constexpr int32 GMaxDecorationActorsPerCell = 400;

// One cell = one chunk XY footprint (so DecorationRadiusChunks reads as a radius in chunks, and a
// decoration's per-cell MaxPerChunk keeps its "per chunk" meaning).
static constexpr int32 DECO_CELL_VOXELS = CHUNK_SIZE;

// Count of in-flight decoration march tasks (for the EndPlay drain). File-scope so the worker lambda's
// RAII guard can touch it without keeping the UObject 'this' alive for that alone.
static std::atomic<int32> GActiveDecoTasks{0};

static FORCEINLINE int32 CellChebyshev(const FIntPoint& A, const FIntPoint& B)
{
    return FMath::Max(FMath::Abs(A.X - B.X), FMath::Abs(A.Y - B.Y));
}

void UVoxelContentManager::Initialize(AActor* InOwner, UVoxelStrateManager* InStrateManager,
                                      UVoxelGenerator* InGenerator, UVoxelSettings* InSettings, int32 InSeed)
{
    Owner = InOwner;
    StrateManager = InStrateManager;
    Generator = InGenerator;
    Settings = InSettings;
    Seed = InSeed;

    if (!PlaneMesh)
    {
        PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    }
}

void UVoxelContentManager::BeginDestroy()
{
    // Backstop: stop any worker from touching us. EndPlay → NotifyShutdown should have already drained.
    bShuttingDown.store(true, std::memory_order_release);
    Super::BeginDestroy();
}

void UVoxelContentManager::NotifyShutdown()
{
    bShuttingDown.store(true, std::memory_order_release);

    // Wait for in-flight march tasks to finish (they check the flag and bail). Timeout to avoid hangs.
    const double Deadline = FPlatformTime::Seconds() + 3.0;
    while (GActiveDecoTasks.load(std::memory_order_relaxed) > 0)
    {
        if (FPlatformTime::Seconds() > Deadline) break;
        FPlatformProcess::Yield();
    }

    FDecoCellResult Discard;
    while (DecoResults.Dequeue(Discard)) {}
    RegionBuilds.Reset();
    CompletedRegions.Reset();
    PendingLaunch.Reset();
    InFlightCells.Reset();
}

//=============================================================================
// Cell ↔ region helpers (region = RxR cells). Floor-division tiles across the origin (negative cells).
//=============================================================================

int32 UVoxelContentManager::RegionSize() const
{
    return FMath::Max(1, Settings ? Settings->DecorationRegionSizeCells : 4);
}

// TRUE floor division (rounds toward -infinity). DO NOT use FMath::DivideAndRoundDown for cell↔region
// mapping: it is `A / B`, which TRUNCATES toward zero for negatives (a UE naming footgun). Cells are
// assigned to a region by the exact inverse `region*R + offset`, so the mapping back MUST floor — with
// truncation, a negative-coord cell is queued under one region but routed back to another → its march
// result is dropped → blank chunk. Truncation == floor for A>=0 and for R==1, which is exactly why only
// NEGATIVE coordinates at R>=2 were affected. R (region size) is always > 0 here.
static FORCEINLINE int32 FloorDivPos(int32 A, int32 R)
{
    return (A >= 0) ? (A / R) : -(((-A) + R - 1) / R);
}

static FORCEINLINE FIntPoint CellToRegion(const FIntPoint& Cell, int32 R)
{
    return FIntPoint(FloorDivPos(Cell.X, R), FloorDivPos(Cell.Y, R));
}

// Region is desired iff its cell footprint intersects the radius-FarR cell box around the player.
static FORCEINLINE bool IsRegionDesired(const FIntPoint& Region, const FIntPoint& PlayerCell,
                                        int32 FarR, int32 R)
{
    const int32 MinX = FloorDivPos(PlayerCell.X - FarR, R);
    const int32 MaxX = FloorDivPos(PlayerCell.X + FarR, R);
    const int32 MinY = FloorDivPos(PlayerCell.Y - FarR, R);
    const int32 MaxY = FloorDivPos(PlayerCell.Y + FarR, R);
    return Region.X >= MinX && Region.X <= MaxX && Region.Y >= MinY && Region.Y <= MaxY;
}

//=============================================================================
// DECORATIONS — distance-based world grid (async march)
//=============================================================================

// Deterministic placement hash. Pure function of cell, column, crossing, entry, seed.
static FORCEINLINE uint32 DecoHash(int32 CX, int32 CY, int32 GX, int32 GY,
                                   int32 Crossing, int32 Entry, uint32 Seed, uint32 Salt)
{
    uint32 H = VoxelHash::Cell(CX, CY, Seed ^ Salt);
    H = VoxelHash::Mix(H ^ ((uint32)GX * 73856093u) ^ ((uint32)GY * 19349663u));
    H = VoxelHash::Mix(H ^ ((uint32)Crossing * 83492791u + 0x9E3779B1u));
    H = VoxelHash::Mix(H ^ ((uint32)Entry * 2654435761u + 40503u));
    return VoxelHash::Mix(H);
}

void UVoxelContentManager::UpdateDecorations(const FVector& PlayerWorldPos)
{
    if (!StrateManager || !Generator || !Settings) return;
    AActor* OwnerActor = Owner.Get();
    if (!OwnerActor) return;

    const FTransform OwnerXf = OwnerActor->GetActorTransform();
    // All strate/water/density queries are in actor-LOCAL space (StrateManager assumes actor origin;
    // the mesher builds geometry from voxel*VOXEL_SIZE in local space). Bring the player local.
    const FVector LocalPlayer = OwnerXf.InverseTransformPosition(PlayerWorldPos);

    // Player's strate band (actor-local cm). No strate (inter-strate gap / outside world) ⇒ no decos.
    float TopZ, BotZ;
    const bool bInStrate = StrateManager->GetStrateUnrealZRange(LocalPlayer.Z, TopZ, BotZ);
    const int32 StrateIndex = bInStrate ? StrateManager->GetStrateIndex(LocalPlayer.Z) : INT32_MIN;

    if (!bInStrate)
    {
        ClearAllDecorations();
        LastStrateIndex = INT32_MIN;
        LastDecoCell    = FIntPoint(INT32_MIN, INT32_MIN);
        return;
    }

    const float CellWorld = (float)DECO_CELL_VOXELS * VOXEL_SIZE;
    const FIntPoint PlayerCell(
        FMath::FloorToInt(LocalPlayer.X / CellWorld),
        FMath::FloorToInt(LocalPlayer.Y / CellWorld));

    // Shared per-update strate context (a strate is a horizontal slab → same for every cell).
    CurrentCtx = FDecoContext();
    CurrentCtx.TopVoxelZ    = TopZ / VOXEL_SIZE;
    CurrentCtx.BottomVoxelZ = BotZ / VOXEL_SIZE;
    CurrentCtx.RepChunkZ    = FMath::FloorToInt(((TopZ + BotZ) * 0.5f / VOXEL_SIZE) / (float)CHUNK_SIZE);
    const FIntVector RepChunk(PlayerCell.X, PlayerCell.Y, CurrentCtx.RepChunkZ);
    CurrentCtx.Def = StrateManager->GetStrateForChunk(RepChunk);
    CurrentCtx.bSurfaceWorld =
        (StrateManager->GetGeneratorTypeForChunk(RepChunk) == ECaveGeneratorType::SurfaceWorld);
    {
        const float Wv = StrateManager->GetWaterLevelWorldZForChunk(RepChunk);
        CurrentCtx.bHasWater   = (Wv != -FLT_MAX);
        CurrentCtx.WaterLocalZ = CurrentCtx.bHasWater ? Wv * VOXEL_SIZE : -FLT_MAX;
    }
    // Strate biome field (XY-global → resolved once; the worker picks the dominant biome per COLUMN).
    CurrentCtx.BiomeCtx = StrateManager->GetBiomeContextForChunk(RepChunk);

    // Build the decoration palette ONCE for this update. With biomes, concatenate every biome's deco list
    // and tag each entry with its context-biome index; the worker resolves a column's biome and rolls only
    // the entries it owns → borders follow the warped-Voronoi field, not the 8 m cell grid (Task 1, §8.5).
    // Without biomes, fall back to the strate's single list tagged -1 (always matches → legacy behaviour).
    CurrentEntries.Reset();
    CurrentEntryBiome.Reset();
    if (CurrentCtx.Def)
    {
        if (CurrentCtx.BiomeCtx.IsValid())
        {
            for (int32 ci = 0; ci < CurrentCtx.BiomeCtx.Biomes.Num(); ++ci)
            {
                const int32 StrateBiomeIdx = CurrentCtx.BiomeCtx.Biomes[ci].Index;
                const UVoxelBiomeDefinition* Bio =
                    CurrentCtx.Def->Biomes.IsValidIndex(StrateBiomeIdx) ? CurrentCtx.Def->Biomes[StrateBiomeIdx] : nullptr;
                // A biome with no decorations inherits the strate's list (still tagged with THIS biome's
                // index so it only fires inside that biome's columns — no cross-biome bleed).
                const TArray<FStrateDecoration>& Src =
                    (Bio && Bio->Decorations.Num() > 0) ? Bio->Decorations : CurrentCtx.Def->Decorations;
                for (const FStrateDecoration& D : Src)
                {
                    CurrentEntries.Add(D);
                    CurrentEntryBiome.Add(ci);
                }
            }
        }
        else
        {
            for (const FStrateDecoration& D : CurrentCtx.Def->Decorations)
            {
                CurrentEntries.Add(D);
                CurrentEntryBiome.Add(-1);   // no biome field → matches the column's ColBiome (-1)
            }
        }
    }

    // Strate change → wipe + force a full rebuild (bumps epoch so old in-flight tasks are discarded).
    if (StrateIndex != LastStrateIndex)
    {
        ClearAllDecorations();
        LastStrateIndex = StrateIndex;
        LastDecoCell    = FIntPoint(INT32_MIN, INT32_MIN);
    }

    if (PlayerCell != LastDecoCell)
    {
        RebuildDesiredCells(PlayerCell);
        LastDecoCell = PlayerCell;
    }

    const int32 FarR = FMath::Max(1, Settings->DecorationRadiusChunks);
    LaunchDecoTasks(PlayerCell);
    ProcessDecoResults(PlayerCell, FarR);
}

void UVoxelContentManager::RebuildDesiredCells(const FIntPoint& PlayerCell)
{
    // REGION-granular streaming. Decoration cells are grouped into RxR regions; a region is the load/
    // unload unit and shares ONE HISM per mesh, so the render thread walks ~R^2 fewer components. A
    // region, once desired, marches ALL of its cells (so it is self-contained and NEVER re-streamed in
    // place while it stays in range — same no-flicker guarantee the per-cell grid had, now per region).
    const int32 FarR = FMath::Max(1, Settings->DecorationRadiusChunks);
    const int32 R    = RegionSize();

    // Desired regions = every region whose footprint touches the radius-FarR cell box around the player.
    const FIntPoint RMin(FloorDivPos(PlayerCell.X - FarR, R),
                         FloorDivPos(PlayerCell.Y - FarR, R));
    const FIntPoint RMax(FloorDivPos(PlayerCell.X + FarR, R),
                         FloorDivPos(PlayerCell.Y + FarR, R));

    TSet<FIntPoint> DesiredRegions;
    DesiredRegions.Reserve((RMax.X - RMin.X + 1) * (RMax.Y - RMin.Y + 1));
    for (int32 ry = RMin.Y; ry <= RMax.Y; ++ry)
    for (int32 rx = RMin.X; rx <= RMax.X; ++rx)
    {
        DesiredRegions.Add(FIntPoint(rx, ry));
    }

    // Unload loaded regions no longer desired (plain DestroyComponent — no per-instance removal).
    {
        TArray<FIntPoint> Loaded; DecoRegions.GetKeys(Loaded);
        for (const FIntPoint& K : Loaded)
        {
            if (!DesiredRegions.Contains(K)) ClearDecorationRegion(K);
        }
    }

    // Start a build for each desired region that isn't already loaded or building. In-progress builds are
    // LEFT to finish even if they fell out of range (their cell tasks are already off-thread); the apply
    // step discards a completed build that is no longer desired (see ProcessDecoResults). Each new build
    // enqueues all RxR of its cells once — a building region is never re-queued (no duplicate launches).
    for (const FIntPoint& Region : DesiredRegions)
    {
        if (DecoRegions.Contains(Region)) continue;     // already applied → leave it (no re-stream)
        if (RegionBuilds.Contains(Region)) continue;    // already marching its cells

        FDecoRegionBuild& Build = RegionBuilds.Add(Region);
        Build.BuildId        = NextBuildId++;
        Build.CellsRemaining = R * R;

        const int32 BaseX = Region.X * R, BaseY = Region.Y * R;
        for (int32 cy = 0; cy < R; ++cy)
        for (int32 cx = 0; cx < R; ++cx)
        {
            PendingLaunch.Add(FIntPoint(BaseX + cx, BaseY + cy));
        }
    }

    // Nearest-first so the region under the player fills in before the fringe. Stale entries (cells whose
    // build was already discarded) are cheaply skipped at launch, so PendingLaunch self-cleans as it drains.
    PendingLaunch.Sort([PlayerCell](const FIntPoint& A, const FIntPoint& B)
    {
        return CellChebyshev(A, PlayerCell) < CellChebyshev(B, PlayerCell);
    });
}

void UVoxelContentManager::LaunchDecoTasks(const FIntPoint& PlayerCell)
{
    if (!CurrentCtx.Def || !Generator) return;
    const int32 MaxConc = Settings->MaxConcurrentDecorationTasks;
    if (MaxConc <= 0)
    {
        // Decorations disabled at runtime — drop all queued/pending build state so nothing is stranded.
        PendingLaunch.Reset();
        RegionBuilds.Reset();
        CompletedRegions.Reset();
        return;
    }

    AActor* OwnerActor = Owner.Get();
    if (!OwnerActor) return;
    const FTransform OwnerXf = OwnerActor->GetActorTransform();

    const int32 R         = RegionSize();
    const int32 Spacing   = FMath::Clamp(Settings->DecorationSpacingVoxels, 1, CHUNK_SIZE);
    const float Step      = (float)FMath::Max(1, Settings->DecorationMarchStepVoxels);
    const int32 MaxCross  = FMath::Max(1, Settings->DecorationMaxCrossingsPerColumn);
    const float ColDepth  = (float)FMath::Max(8, Settings->DecorationColumnDepthVoxels);

    while (PendingLaunch.Num() > 0 && InFlightCells.Num() < MaxConc)
    {
        const FIntPoint Cell = PendingLaunch[0];
        PendingLaunch.RemoveAt(0);

        if (InFlightCells.Contains(Cell)) continue;

        // The cell's region build drives completion. If it's gone (region applied or discarded since this
        // cell was queued), drop the cell — no range check here: a region intentionally marches all its
        // cells (some sit just past FarR), and discarding the build is the only "no longer wanted" signal.
        const FIntPoint Region = CellToRegion(Cell, R);
        FDecoRegionBuild* Build = RegionBuilds.Find(Region);
        if (!Build) continue;
        const uint32 BuildId = Build->BuildId;

        // The decoration palette (all biomes' lists, flattened + tagged) is built ONCE per update in
        // UpdateDecorations; the per-COLUMN biome pick happens on the worker. Snapshot the flat list +
        // tags for this cell's task (the biome context rides in Ctx).
        if (CurrentEntries.Num() == 0)
        {
            MarkCellDone(Region, Cell, BuildId);   // empty cell still counts toward the region's completion
            continue;
        }

        TArray<FStrateDecoration> EntriesCopy   = CurrentEntries;     // snapshot for the worker + the spawner
        TArray<int32>             EntryBiomeCopy = CurrentEntryBiome;  // parallel: ctx-biome owner per entry
        const FDecoContext Ctx = CurrentCtx;                // PODs only used on the worker
        const uint32 LocalSeed = (uint32)Seed;
        UVoxelGenerator* Gen = Generator;

        InFlightCells.Add(Cell);
        GActiveDecoTasks.fetch_add(1, std::memory_order_relaxed);

        UE::Tasks::Launch(TEXT("DecoMarch"),
            [this, Gen, OwnerXf, Cell, Ctx, LocalSeed, Spacing, Step, MaxCross, ColDepth, BuildId,
             Entries = MoveTemp(EntriesCopy), EntryBiome = MoveTemp(EntryBiomeCopy)]() mutable
            {
                struct FGuard { ~FGuard() { GActiveDecoTasks.fetch_sub(1, std::memory_order_relaxed); } } Guard;

                if (bShuttingDown.load(std::memory_order_relaxed)) return;

                FDecoCellResult Result;
                Result.Cell    = Cell;
                Result.BuildId = BuildId;
                Result.Entries = MoveTemp(Entries);
                BuildCellSpawns(Gen, OwnerXf, Cell, Ctx, Result.Entries, EntryBiome, LocalSeed,
                                Spacing, Step, MaxCross, ColDepth, Result.Spawns);

                if (!bShuttingDown.load(std::memory_order_relaxed))
                {
                    DecoResults.Enqueue(MoveTemp(Result));
                }
            }, UE::Tasks::ETaskPriority::BackgroundNormal);
    }
}

// ---- WORKER THREAD: find each column's surface points → spawn commands. ----
void UVoxelContentManager::BuildCellSpawns(const UVoxelGenerator* Gen, const FTransform& OwnerXf,
                                           const FIntPoint& Cell, const FDecoContext& Ctx,
                                           const TArray<FStrateDecoration>& Entries,
                                           const TArray<int32>& EntryBiome, uint32 InSeed,
                                           int32 Spacing, float Step, int32 MaxCrossings, float ColumnDepth,
                                           TArray<FDecoSpawn>& OutSpawns)
{
    if (!Gen || Entries.Num() == 0) return;

    const int32 PerAxis = FMath::Max(1, CHUNK_SIZE / Spacing);
    const int32 CellOriginVX = Cell.X * CHUNK_SIZE;
    const int32 CellOriginVY = Cell.Y * CHUNK_SIZE;

    TArray<int32> EntryCount; EntryCount.Init(0, Entries.Num());
    int32 TotalActors = 0;

    // Per-COLUMN biome cache: ResolveBiomeSampleAt's noise-heavy cell classification is box-validated
    // (one rebuild per chunk footprint), so resolving the dominant biome at every column in this cell is
    // cheap. The cache is local to this worker task (determinism-safe — pure function of XY/seed/Ctx).
    FChunkBiomeCache BiomeCache;
    const bool bHasBiomes = Ctx.BiomeCtx.IsValid();

    auto D = [&](float VX, float VY, float VZ) { return Gen->GetDensityAt(VX, VY, VZ); };

    // Shared: roll every decoration entry at one surface point (voxel XY, voxel Z, outward world normal)
    // and append the passing ones to OutSpawns. CrossingIdx salts the hash so stacked surfaces differ.
    // ColBiome = the column's dominant context-biome index (-1 when biomes are off); an entry is rolled
    // only if it belongs to that biome (EntryBiome[EntryIdx] == ColBiome) → organic, per-column borders.
    auto PlaceAtCrossing = [&](float VX, float VY, int32 gx, int32 gy, float ZC,
                               const FVector& NormalWorld, int32 CrossingIdx, int32 ColBiome)
    {
        const bool bFloor   = NormalWorld.Z >  0.5f;
        const bool bCeiling = NormalWorld.Z < -0.5f;
        const bool bWall    = !bFloor && !bCeiling;

        const FVector LocalPos(VX * VOXEL_SIZE, VY * VOXEL_SIZE, ZC * VOXEL_SIZE);
        const FVector PosWorld = OwnerXf.TransformPosition(LocalPos);
        const bool bBelowWater = Ctx.bHasWater && (LocalPos.Z < Ctx.WaterLocalZ);

        for (int32 EntryIdx = 0; EntryIdx < Entries.Num(); ++EntryIdx)
        {
            // Per-column biome gate: only this column's dominant biome owns its entries. -1-tagged
            // entries (no biome field) match the -1 ColBiome, so the legacy single-list path is intact.
            if (EntryBiome[EntryIdx] != ColBiome) continue;

            const FStrateDecoration& Deco = Entries[EntryIdx];
            const bool bInstanced = (Deco.InstancedMesh != nullptr);
            if (!bInstanced && !Deco.ActorClass) continue;
            if (Deco.SpawnDensity <= 0.0f) continue;
            if (EntryCount[EntryIdx] >= Deco.MaxPerChunk) continue;
            if (!bInstanced && TotalActors >= GMaxDecorationActorsPerCell) continue;

            bool bMatches = true;
            switch (Deco.SurfacePlacement)
            {
            case ESurfaceType::Floor:   bMatches = bFloor;   break;
            case ESurfaceType::Wall:    bMatches = bWall;    break;
            case ESurfaceType::Ceiling: bMatches = bCeiling; break;
            default:                    bMatches = true;     break;
            }
            if (!bMatches) continue;

            // Strict-wall overhang gate: a "wall" point also covers surfaces that lean slightly downward
            // (N.Z in [-0.5, 0)). For props flagged wall-only-upright, drop those so overhangs don't take
            // wall decals. Applies whenever the point IS a wall (independent of Floor/Wall/Any setting).
            if (bWall && Deco.bWallExcludeOverhangs && NormalWorld.Z < 0.0f) continue;

            // Surface-tilt gate: tilt = acos(|N.Z|) (0 = flat, 90 = vertical). Skip surfaces steeper than
            // MaxSlopeAngle. cos is monotone-decreasing, so |N.Z| < cos(MaxSlope) ⇔ tilt > MaxSlope.
            // Guarded so the default (90°, cos = 0) costs no trig and never rejects anything.
            if (Deco.MaxSlopeAngle < 89.99f &&
                FMath::Abs(NormalWorld.Z) < FMath::Cos(FMath::DegreesToRadians(Deco.MaxSlopeAngle)))
            {
                continue;
            }

            // Lower-bound tilt gate (companion to the above): skip surfaces FLATTER than MinSlopeAngle.
            // tilt < MinSlope ⇔ |N.Z| > cos(MinSlope). Guarded so the default (0°, cos = 1) never rejects.
            if (Deco.MinSlopeAngle > 0.01f &&
                FMath::Abs(NormalWorld.Z) > FMath::Cos(FMath::DegreesToRadians(Deco.MinSlopeAngle)))
            {
                continue;
            }

            const uint32 H = DecoHash(Cell.X, Cell.Y, gx, gy, CrossingIdx, EntryIdx, InSeed, 0xDEC0u);
            if (VoxelHash::ToFloat01(H) > Deco.SpawnDensity) continue;

            if (Deco.bRequireWaterRelative && Ctx.bHasWater)
            {
                if (bBelowWater != Deco.bPlaceBelowWater) continue;
            }

            const FVector SpawnPos = PosWorld + NormalWorld * Deco.SurfaceOffset;
            FQuat BaseQ = Deco.bAlignToSurface
                ? FRotationMatrix::MakeFromZ(NormalWorld).ToQuat()
                : FQuat::Identity;
            if (Deco.bRandomYaw)
            {
                // Roll within [MinYaw, MaxYaw]; the default 0..360 reproduces the legacy full-turn roll
                // bit-for-bit (Lerp(0,360,t)° == t·2π rad), so existing assets are unchanged.
                const float YawT   = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x59415721u));
                const float Yaw    = FMath::DegreesToRadians(FMath::Lerp(Deco.MinYaw, Deco.MaxYaw, YawT));
                const FVector Axis = Deco.bAlignToSurface ? NormalWorld : FVector::UpVector;
                BaseQ = FQuat(Axis, Yaw) * BaseQ;
            }
            const float ScaleT = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x5CA1E000u));
            const float Scale  = FMath::Lerp(Deco.MinScale, Deco.MaxScale, ScaleT);

            FDecoSpawn& Out = OutSpawns.AddDefaulted_GetRef();
            Out.EntryIdx   = EntryIdx;
            Out.bInstanced = bInstanced;
            Out.Xf         = FTransform(BaseQ, SpawnPos, FVector(Scale));
            ++EntryCount[EntryIdx];
            if (!bInstanced) ++TotalActors;
        }
    };

    for (int32 gy = 0; gy < PerAxis; ++gy)
    for (int32 gx = 0; gx < PerAxis; ++gx)
    {
        // INTEGER voxel jitter: columns land on integer XY so the generator's surface-column cache
        // (T1.a, §8.10) applies — FRACTIONAL XY recomputes the noise-heavy heightfield+biome on EVERY
        // sample. A 25 cm grid offset is imperceptible; random yaw/scale still breaks up the regularity.
        const uint32 HJ = DecoHash(Cell.X, Cell.Y, gx, gy, -1, 0, InSeed, 0x10C0u);
        const int32 JX = FMath::Min(Spacing - 1, (int32)(VoxelHash::ToFloat01(HJ) * (float)Spacing));
        const int32 JY = FMath::Min(Spacing - 1, (int32)(VoxelHash::ToFloat01(VoxelHash::Mix(HJ ^ 0x68BC21EBu)) * (float)Spacing));
        const float VX = (float)(CellOriginVX + gx * Spacing + JX);
        const float VY = (float)(CellOriginVY + gy * Spacing + JY);

        // Resolve the column's biome ONCE (constant over the column's whole Z range). -1 when the strate
        // has no biome field → matches the -1-tagged legacy entries.
        int32 ColBiome = -1;
        if (bHasBiomes)
        {
            const FBiomeSample BS = Gen->ResolveBiomeSampleAt(VX, VY, Ctx.RepChunkZ, Ctx.BiomeCtx, BiomeCache);
            ColBiome = BS.DominantIndex;   // index into Ctx.BiomeCtx.Biomes == the entry tag

            // SOFTEN THE BORDER: a hard dominant pick still switches deco sets on a crisp line. In the
            // blend band (NeighborWeight rises 0 → ~0.5 toward the shared border) flip a hash-decided
            // fraction of columns to the NEIGHBOUR biome, so the two deco sets DITHER across the seam
            // instead of snapping. Deterministic (pure hash of cell/column/seed) → no flicker/perf cost.
            if (BS.NeighborWeight > 0.0f && BS.NeighborIndex >= 0)
            {
                const uint32 HBlend = DecoHash(Cell.X, Cell.Y, gx, gy, -7, 0, InSeed, 0xB1E2u);
                if (VoxelHash::ToFloat01(HBlend) < BS.NeighborWeight) { ColBiome = BS.NeighborIndex; }
            }
        }

        if (Ctx.bSurfaceWorld)
        {
            // HEIGHTFIELD ORACLE — O(1)/column instead of marching the whole band. Query the surface
            // (+ 4 neighbours for the gradient normals), then verify with ONE density sample so we skip
            // columns carved away by passages / the (0,0) spine / player diffs (the oracle is the raw
            // heightfield and doesn't know about carving).
            float hC, cC, hXp, cXp, hXm, cXm, hYp, cYp, hYm, cYm;
            if (!Gen->GetSurfaceHeightAt(VX, VY, Ctx.RepChunkZ, hC, cC)) continue;
            Gen->GetSurfaceHeightAt(VX + 1.0f, VY, Ctx.RepChunkZ, hXp, cXp);
            Gen->GetSurfaceHeightAt(VX - 1.0f, VY, Ctx.RepChunkZ, hXm, cXm);
            Gen->GetSurfaceHeightAt(VX, VY + 1.0f, Ctx.RepChunkZ, hYp, cYp);
            Gen->GetSurfaceHeightAt(VX, VY - 1.0f, Ctx.RepChunkZ, hYm, cYm);

            // Floor (terrain top): outward normal = (-dH/dx, -dH/dy, 1).
            if (hC >= Ctx.BottomVoxelZ && hC <= Ctx.TopVoxelZ && D(VX, VY, hC) <= 0.5f)
            {
                const float dHdx = (hXp - hXm) * 0.5f;
                const float dHdy = (hYp - hYm) * 0.5f;
                FVector N = OwnerXf.TransformVectorNoScale(FVector(-dHdx, -dHdy, 1.0f)).GetSafeNormal();
                if (N.IsNearlyZero()) N = FVector::UpVector;
                PlaceAtCrossing(VX, VY, gx, gy, hC, N, 0, ColBiome);
            }

            // Sky-cap ceiling underside: outward normal = (dC/dx, dC/dy, -1). Only if open space below.
            if (cC > hC + 1.0f && cC <= Ctx.TopVoxelZ && D(VX, VY, cC) <= 0.5f)
            {
                const float dCdx = (cXp - cXm) * 0.5f;
                const float dCdy = (cYp - cYm) * 0.5f;
                FVector N = OwnerXf.TransformVectorNoScale(FVector(dCdx, dCdy, -1.0f)).GetSafeNormal();
                if (N.IsNearlyZero()) N = FVector::DownVector;
                PlaceAtCrossing(VX, VY, gx, gy, cC, N, 1, ColBiome);
            }
            continue;
        }

        // NON-SURFACE archetypes (caves/shafts/islands): ray-march the density column for crossings.
        float PrevD = D(VX, VY, Ctx.TopVoxelZ);
        int32 Crossings = 0;
        bool  bSeenAir = (PrevD >= 0.0f);   // GetDensityAt >= 0 == air
        float SolidRun = 0.0f;              // contiguous solid voxels since the last open air

        for (float Z = Ctx.TopVoxelZ - Step; Z >= Ctx.BottomVoxelZ && Crossings < MaxCrossings; Z -= Step)
        {
            const float Dz = D(VX, VY, Z);
            if ((PrevD >= 0.0f) != (Dz >= 0.0f))   // straddles IsoLevel 0 (air ↔ solid)
            {
                // Bisection-refine the crossing Z between Z (lo, Dz) and Z+Step (hi, PrevD).
                float ZLo = Z, DLo = Dz, ZHi = Z + Step, DHi = PrevD;
                for (int32 It = 0; It < 4; ++It)
                {
                    const float ZM = 0.5f * (ZLo + ZHi);
                    const float DM = D(VX, VY, ZM);
                    if ((DM >= 0.0f) == (DHi >= 0.0f)) { ZHi = ZM; DHi = DM; }
                    else                               { ZLo = ZM; DLo = DM; }
                }
                const float Denom = (DLo - DHi);
                const float T = (FMath::Abs(Denom) > KINDA_SMALL_NUMBER) ? (DLo / Denom) : 0.5f;
                const float ZC = ZLo + (ZHi - ZLo) * T;

                const FVector LocalGrad(
                    D(VX + 1.0f, VY, ZC) - D(VX - 1.0f, VY, ZC),
                    D(VX, VY + 1.0f, ZC) - D(VX, VY - 1.0f, ZC),
                    D(VX, VY, ZC + 1.0f) - D(VX, VY, ZC - 1.0f));
                FVector NormalWorld = OwnerXf.TransformVectorNoScale(LocalGrad).GetSafeNormal();
                if (NormalWorld.IsNearlyZero()) NormalWorld = FVector::UpVector;

                PlaceAtCrossing(VX, VY, gx, gy, ZC, NormalWorld, Crossings, ColBiome);
                ++Crossings;
            }

            // Once past into the open space, stop after a long bedrock run below it (skips solid rock to
            // the strate floor; caves reset on each air gap so layered floors are still found).
            if (Dz >= 0.0f) { bSeenAir = true; SolidRun = 0.0f; }
            else            { SolidRun += Step; }
            if (bSeenAir && SolidRun > ColumnDepth) break;

            PrevD = Dz;
        }
    }
}

// ---- GAME THREAD: drain finished marches → merge into region builds, apply completed regions budgeted. ----
void UVoxelContentManager::ProcessDecoResults(const FIntPoint& PlayerCell, int32 FarR)
{
    // Drain every finished cell march and fold it into its region build. Merging is cheap (transform
    // appends) so it isn't budgeted; the expensive HISM build is budgeted below at region granularity.
    FDecoCellResult R;
    while (DecoResults.Dequeue(R))
    {
        InFlightCells.Remove(R.Cell);   // free the concurrency slot regardless of whether it still matters
        MergeCellResult(R);
    }

    // Apply completed regions (one batched HISM-per-mesh build), budgeted. A region whose build finished
    // but is no longer desired (player moved on while it marched) is discarded instead of applied — that
    // keeps an out-of-range region from flashing in for a frame before the next unload pass.
    const int32 R_ = RegionSize();
    const int32 Budget = FMath::Max(1, Settings->MaxDecorationCellsPerFrame);
    int32 Applied = 0;
    while (CompletedRegions.Num() > 0 && Applied < Budget)
    {
        const FIntPoint Region = CompletedRegions[0];
        CompletedRegions.RemoveAt(0);

        FDecoRegionBuild* Build = RegionBuilds.Find(Region);
        if (!Build) continue;   // already cleared

        if (!IsRegionDesired(Region, PlayerCell, FarR, R_))
        {
            RegionBuilds.Remove(Region);   // wandered out of range while building → drop it unbuilt
            continue;
        }

        ApplyRegion(Region, *Build);
        RegionBuilds.Remove(Region);
        ++Applied;
    }
}

// Fold one finished cell's spawns into its region build, then mark the cell accounted for. A result whose
// region build is gone or whose BuildId no longer matches (region was cleared + re-marched) is discarded.
void UVoxelContentManager::MergeCellResult(const FDecoCellResult& Result)
{
    const FIntPoint Region = CellToRegion(Result.Cell, RegionSize());
    FDecoRegionBuild* Build = RegionBuilds.Find(Region);
    if (!Build || Build->BuildId != Result.BuildId)
    {
        return;
    }
    // Already counted this cell (a duplicate task for the same cell+build landed first) → drop this
    // result whole, or we'd append its spawns twice (double decorations at the same spots).
    if (Build->AccountedCells.Contains(Result.Cell))
    {
        return;
    }

    for (const FDecoSpawn& S : Result.Spawns)
    {
        if (!Result.Entries.IsValidIndex(S.EntryIdx)) continue;
        const FStrateDecoration& Deco = Result.Entries[S.EntryIdx];

        if (S.bInstanced)
        {
            if (!Deco.InstancedMesh) continue;
            // Bucket by MESH so cells (and biomes) sharing a mesh collapse into one region HISM. The first
            // contributor sets the render tuning (cull/shadow/scale) for the whole region's instances.
            FRegionMeshBucket& Bucket = Build->MeshBuckets.FindOrAdd(Deco.InstancedMesh);
            if (Bucket.Xforms.Num() == 0) { Bucket.Deco = Deco; }
            Bucket.Xforms.Add(S.Xf);
        }
        else if (Deco.ActorClass)
        {
            FRegionActorSpawn& A = Build->ActorSpawns.AddDefaulted_GetRef();
            A.ActorClass = Deco.ActorClass;
            A.Xf         = S.Xf;
        }
    }

    MarkCellDone(Region, Result.Cell, Result.BuildId);
}

// Account one cell against its region — IDEMPOTENT per cell, so a duplicate task for the same cell can't
// double-decrement and apply the region early (which left a permanently-empty chunk until a regen). Queues
// the region for apply once every distinct cell has reported.
void UVoxelContentManager::MarkCellDone(const FIntPoint& Region, const FIntPoint& Cell, uint32 BuildId)
{
    FDecoRegionBuild* Build = RegionBuilds.Find(Region);
    if (!Build || Build->BuildId != BuildId) return;

    bool bAlreadyAccounted = false;
    Build->AccountedCells.Add(Cell, &bAlreadyAccounted);
    if (bAlreadyAccounted) return;   // this cell already counted → ignore the duplicate

    if (--Build->CellsRemaining <= 0)
    {
        CompletedRegions.Add(Region);   // ready for budgeted apply in ProcessDecoResults
    }
}

// Build the region's components: one HISM per mesh (all cells merged → one batched AddInstances), actors
// spawned inline. Moves the region into DecoRegions; the build is removed by the caller.
void UVoxelContentManager::ApplyRegion(const FIntPoint& Region, FDecoRegionBuild& Build)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_DecoApply);   // total game-thread cost to apply one region

    AActor* OwnerActor = Owner.Get();
    if (!OwnerActor) return;
    UWorld* World = OwnerActor->GetWorld();
    if (!World) return;

    FDecoRegionContent& Content = DecoRegions.Add(Region);

    // Non-instanced actors — spawn each (no batch path). Decorations live only in the player's strate
    // (the march is strate-bounded), so their lights are always legitimately visible — no extra culling.
    for (const FRegionActorSpawn& A : Build.ActorSpawns)
    {
        if (!A.ActorClass) continue;
        FActorSpawnParameters SpawnParams;
        SpawnParams.Owner = OwnerActor;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        if (AActor* NewActor = World->SpawnActor<AActor>(A.ActorClass, A.Xf, SpawnParams))
        {
            Content.Actors.Add(NewActor);
        }
    }

    // One HISM per mesh for the ENTIRE region — the render-thread win: hundreds of per-cell components
    // collapse to a handful per region, so InitViews walks far fewer primitives every frame.
    for (TPair<TWeakObjectPtr<UStaticMesh>, FRegionMeshBucket>& Pair : Build.MeshBuckets)
    {
        FRegionMeshBucket& Bucket = Pair.Value;
        UStaticMesh* Mesh = Pair.Key.Get();
        if (!Mesh || Bucket.Xforms.Num() == 0) continue;
        const FStrateDecoration& Deco = Bucket.Deco;

        UHierarchicalInstancedStaticMeshComponent* HISM =
            NewObject<UHierarchicalInstancedStaticMeshComponent>(OwnerActor);
        HISM->SetStaticMesh(Mesh);
        // STATIC, not Movable: decorations are placed once and never move, so Static lets the renderer
        // CACHE their mesh draw commands (they drop out of the per-frame dynamic-primitive gather — the
        // dominant render-thread cost once component count was solved) AND lets VSM cache their shadows.
        // Mirrors the terrain tile + root mobility rationale (VoxelWorld.cpp). Must precede RegisterComponent.
        HISM->SetMobility(EComponentMobility::Static);
        HISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

        // Per-entry render tuning — what makes dense groundcover affordable. Set BEFORE RegisterComponent
        // so the render proxy is created once with the final state (no rebuild):
        //   • CullDistance bounds GPU cost — grass is drawn only near the player even when placed thickly.
        //   • bCastShadow off removes the dominant cost of dense instanced foliage.
        HISM->SetCastShadow(Deco.bCastShadow);
        if (Deco.CullDistance > 0.0f)
        {
            const int32 End   = FMath::Max(1, (int32)Deco.CullDistance);
            const int32 Start = FMath::Max(1, (int32)(Deco.CullDistance * 0.8f));
            HISM->SetCullDistances(Start, End);   // fade band 0.8x→1.0x, then gone
        }

        {
            // Scene-proxy creation. Now amortised over a whole region rather than churned per cell.
            TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_DecoHISMRegister);
            HISM->RegisterComponent();
            HISM->AttachToComponent(OwnerActor->GetRootComponent(),
                FAttachmentTransformRules::KeepRelativeTransform);
        }
        {
            // Cluster-tree build over the WHOLE region's instances in one shot (scales with instance
            // count, but built once per region instead of once per cell).
            TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_DecoAddInstances);
            HISM->AddInstances(Bucket.Xforms, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
        }
        Content.Instances.Add(HISM);
    }
}

void UVoxelContentManager::ClearDecorationRegion(const FIntPoint& Region)
{
    FDecoRegionContent* Content = DecoRegions.Find(Region);
    if (!Content) return;

    for (const TWeakObjectPtr<AActor>& A : Content->Actors)
    {
        if (AActor* Act = A.Get()) { Act->Destroy(); }
    }
    for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& C : Content->Instances)
    {
        if (UHierarchicalInstancedStaticMeshComponent* Comp = C.Get()) { Comp->DestroyComponent(); }
    }
    DecoRegions.Remove(Region);
}

void UVoxelContentManager::ClearAllDecorations()
{
    TArray<FIntPoint> Keys; DecoRegions.GetKeys(Keys);
    for (const FIntPoint& K : Keys) ClearDecorationRegion(K);

    RegionBuilds.Reset();      // abandon any in-progress builds
    CompletedRegions.Reset();
    PendingLaunch.Reset();
    InFlightCells.Reset();
    // Drain any results already enqueued by in-flight tasks. No epoch bump needed: their BuildIds are now
    // gone from RegionBuilds, so any straggler result is discarded on merge; new builds get fresh BuildIds.
    FDecoCellResult Discard;
    while (DecoResults.Dequeue(Discard)) {}
}

//=============================================================================
// WATER — tile-driven, level-0 only
//=============================================================================

void UVoxelContentManager::UpdateWater(const FVector& PlayerWorldPos)
{
    if (!StrateManager || !PlaneMesh) return;
    AActor* OwnerActor = Owner.Get();
    if (!OwnerActor) return;

    const FTransform Xf = OwnerActor->GetActorTransform();
    const FVector LocalPos = Xf.InverseTransformPosition(PlayerWorldPos);

    // Player chunk (voxel→chunk) for the strate / water-level lookup.
    const float ChunkWorld = (float)CHUNK_SIZE * VOXEL_SIZE;   // 1 chunk footprint in cm
    const FIntVector PlayerChunk(
        FMath::FloorToInt(LocalPos.X / ChunkWorld),
        FMath::FloorToInt(LocalPos.Y / ChunkWorld),
        FMath::FloorToInt(LocalPos.Z / ChunkWorld));

    const float WaterVoxelZ = StrateManager->GetWaterLevelWorldZForChunk(PlayerChunk);
    if (WaterVoxelZ == -FLT_MAX)   // current strate has no water → hide the plane
    {
        if (WaterPlane) { WaterPlane->SetVisibility(false); }
        LastWaterZ    = -FLT_MAX;
        LastWaterCell = FIntPoint(INT32_MIN, INT32_MIN);
        return;
    }

    // Snap the plane centre to a coarse cell so it only repositions when the player crosses it
    // (avoids per-frame churn; a uniform plane sliding is invisible anyway as long as the water
    // material pans on WORLD position rather than mesh UVs).
    const FIntPoint Cell(
        FMath::FloorToInt(LocalPos.X / ChunkWorld),
        FMath::FloorToInt(LocalPos.Y / ChunkWorld));

    if (WaterPlane && WaterPlane->IsVisible() && WaterVoxelZ == LastWaterZ && Cell == LastWaterCell)
        return;   // nothing changed

    if (!WaterPlane)
    {
        WaterPlane = NewObject<UStaticMeshComponent>(OwnerActor);
        WaterPlane->SetStaticMesh(PlaneMesh);
        WaterPlane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        WaterPlane->SetCastShadow(false);
        WaterPlane->SetMobility(EComponentMobility::Movable);   // it follows the player
        WaterPlane->RegisterComponent();
        WaterPlane->AttachToComponent(OwnerActor->GetRootComponent(),
                                      FAttachmentTransformRules::KeepRelativeTransform);
    }

    // Cover the whole view to the horizon (one draw, so be generous). Engine plane is 100 uu.
    const float CoverWorld = Settings
        ? (float)FMath::Max(4, Settings->ViewDistanceXY + 4) * 2.0f * ChunkWorld
        : 200000.0f;
    WaterPlane->SetWorldScale3D(FVector(CoverWorld / 100.0f, CoverWorld / 100.0f, 1.0f));

    const FVector LocalCenter(((float)Cell.X + 0.5f) * ChunkWorld,
                              ((float)Cell.Y + 0.5f) * ChunkWorld,
                              WaterVoxelZ * VOXEL_SIZE);
    WaterPlane->SetWorldLocation(Xf.TransformPosition(LocalCenter));
    WaterPlane->SetVisibility(true);

    if (const UVoxelStrateDefinition* Def = StrateManager->GetStrateForChunk(PlayerChunk))
    {
        if (Def->WaterMaterial) { WaterPlane->SetMaterial(0, Def->WaterMaterial); }
    }

    LastWaterZ    = WaterVoxelZ;
    LastWaterCell = Cell;
}

//=============================================================================
// CLEAR ALL
//=============================================================================

void UVoxelContentManager::ClearAll()
{
    ClearAllDecorations();

    if (WaterPlane) { WaterPlane->DestroyComponent(); WaterPlane = nullptr; }
    LastWaterZ    = -FLT_MAX;
    LastWaterCell = FIntPoint(INT32_MIN, INT32_MIN);

    // Force a full decoration rebuild on the next update.
    LastDecoCell    = FIntPoint(INT32_MIN, INT32_MIN);
    LastStrateIndex = INT32_MIN;
}

//=============================================================================
// DIAGNOSTIC — decoration streaming state under a point
//=============================================================================

void UVoxelContentManager::QueryDecoDebugAt(const FVector& LocalPos, bool& bApplied, int32& InstanceCount,
                                            bool& bBuilding, int32& CellsAccounted, int32& CellsTotal,
                                            int32& LiveMarchSpawns, int32& InstancesInCell) const
{
    bApplied = false; InstanceCount = 0; bBuilding = false; CellsAccounted = 0;
    LiveMarchSpawns = -1; InstancesInCell = 0;

    const int32 R = RegionSize();
    CellsTotal = R * R;

    // World-LOCAL XY → deco cell → region (same math the scatter uses).
    const float CellWorld = (float)DECO_CELL_VOXELS * VOXEL_SIZE;
    const FIntPoint Cell(FMath::FloorToInt(LocalPos.X / CellWorld),
                         FMath::FloorToInt(LocalPos.Y / CellWorld));
    const FIntPoint Region = CellToRegion(Cell, R);

    // Actor-LOCAL XY footprint of the probed cell (instances are stored component-local; the HISM sits at
    // the actor transform with identity relative, so component-local == actor-local cell space).
    const float CellMinX = (float)Cell.X * CellWorld, CellMaxX = CellMinX + CellWorld;
    const float CellMinY = (float)Cell.Y * CellWorld, CellMaxY = CellMinY + CellWorld;

    if (const FDecoRegionContent* Content = DecoRegions.Find(Region))
    {
        bApplied = true;
        for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& C : Content->Instances)
        {
            const UHierarchicalInstancedStaticMeshComponent* Comp = C.Get();
            if (!Comp) continue;
            const int32 N = Comp->GetInstanceCount();
            InstanceCount += N;
            // Count the ones actually inside the probed cell → tells a blank cell apart from a blank region.
            for (int32 i = 0; i < N; ++i)
            {
                FTransform Xf;
                if (!Comp->GetInstanceTransform(i, Xf, /*bWorldSpace=*/false)) continue;
                const FVector P = Xf.GetLocation();
                if (P.X >= CellMinX && P.X < CellMaxX && P.Y >= CellMinY && P.Y < CellMaxY)
                {
                    ++InstancesInCell;
                }
            }
        }
    }
    if (const FDecoRegionBuild* Build = RegionBuilds.Find(Region))
    {
        bBuilding      = true;
        CellsAccounted = Build->AccountedCells.Num();
    }

    // PER-CELL decisive probe: re-run the march for THIS cell synchronously with the current strate
    // context (set each UpdateDecorations). Same inputs the worker uses → byte-identical result, so it
    // reports exactly what the scatter decides for this cell right now. Only valid when the probed point
    // shares the player's current strate (CurrentCtx/CurrentEntries reflect that); else leave -1.
    AActor* OwnerActor = Owner.Get();
    if (Generator && Settings && OwnerActor && CurrentCtx.Def && CurrentEntries.Num() > 0)
    {
        const int32 Spacing  = FMath::Clamp(Settings->DecorationSpacingVoxels, 1, CHUNK_SIZE);
        const float Step     = (float)FMath::Max(1, Settings->DecorationMarchStepVoxels);
        const int32 MaxCross = FMath::Max(1, Settings->DecorationMaxCrossingsPerColumn);
        const float ColDepth = (float)FMath::Max(8, Settings->DecorationColumnDepthVoxels);

        TArray<FDecoSpawn> Spawns;
        BuildCellSpawns(Generator, OwnerActor->GetActorTransform(), Cell, CurrentCtx,
                        CurrentEntries, CurrentEntryBiome, (uint32)Seed,
                        Spacing, Step, MaxCross, ColDepth, Spawns);
        LiveMarchSpawns = Spawns.Num();
    }
}
