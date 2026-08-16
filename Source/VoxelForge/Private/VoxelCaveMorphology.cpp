// VoxelCaveMorphology.cpp
// Hash-based room/tunnel generation and SDF evaluation.
//
// TWO-PHASE EVALUATION:
// Phase 1 — BuildChunkCache: collects rooms, builds backbone, resolves tunnels.
//           Called ONCE per chunk (~1 time per 32³ = 32,768 voxels).
// Phase 2 — EvaluateSDFCached: evaluates room/tunnel SDFs with distance culling.
//           Called PER VOXEL using the cached data.
//
// FEATURES:
// - Origin room: guaranteed large room at (0,0) per strate — the hub / (0,0) spine
// - Hash-based rooms: ellipsoid, rounded box, and capsule shapes
// - Tunnels: tapered capsules with curved paths (midpoint warping)
// - Horizontal bias: tunnels prefer horizontal connections
// - Endpoint Z offset: tunnels enter rooms at different heights
// - Per-room/tunnel distance culling: skip SDFs that can't affect this voxel
//
// CROSS-CHUNK DETERMINISM (the invariant that prevents seams):
// The room/tunnel GRAPH must reconstruct IDENTICALLY in every chunk whose voxels a
// tunnel touches. Room geometry is a pure hash of its cell, so that half is trivially
// identical. The fragile half is the EXISTENCE decision (the nearest-neighbor
// backbone), which historically depended on the per-chunk window of collected rooms.
// We now separate two regions (see BuildChunkCache):
//   - COLLECT region (wide): every room whose NN-candidate set could influence a
//     tunnel touching this chunk. Connectivity is decided over THIS set, so the
//     decision is window-invariant.
//   - STORE region (tight): only rooms/tunnels that can actually reach a voxel in
//     this chunk are kept for the per-voxel loop, so hot-path cost stays low.

#include "VoxelCaveMorphology.h"
#include "VoxelTypes.h"          // Pour VOXEL_NOISE_SCALE, SmoothStep01
#include "VoxelStrateTypes.h"
#include "VoxelTerrainOpDefinition.h"

//=============================================================================
// INTERNAL: Full room data used during cache building only.
// The CellX/CellY fields are needed for tunnel pair hashing but NOT for
// per-voxel SDF evaluation, so they don't go into the cached FCachedRoom.
//=============================================================================
struct FBuildRoom
{
    FVector Center;         // World position of the room center
    float RadiusXY;         // Horizontal radius
    float RadiusZ;          // Vertical radius
    int32 CellX, CellY;    // Grid cell (needed for pair hash during tunnel decisions)
    uint32 Hash;            // Cell hash (for shape selection + tunnel property derivation)
    bool bIsOrigin;         // True for the origin room at (0,0)
    bool bStore;            // True if this room can affect a voxel in THIS chunk
                            // (collected for connectivity decisions either way).
};

//=============================================================================
// INTERNAL: Shared hash-placement skeleton for the per-room baked features
// (pits / chimneys / columns). Squelette commun de placement par hash — les
// trois boucles de bake étaient des copies quasi identiques de ce motif.
//
// Rolls EXACTLY the hash chain the hand-written loops used (bit-identical):
//   H  = Mix(RoomHash ^ (SaltBase + i * SaltStep))  → density gate
//   H2 = Mix(H ^ Salt2)                             → XY offset (X: H2, Y: Mix(H2))
//   H3 = Mix(H2 ^ Salt3)                            → radius lerp [MinRadius, MaxRadius]
// Type-specific work (Z anchor, flare, bounds, struct fill) lives in the Emit
// lambda; it receives H3 so pits/chimneys can chain their 4th hash from it.
//=============================================================================
template <typename FEmit>
static void BakeRoomFeature(
    const FCachedRoom& CR,
    int32 MaxCount, float Density,
    uint32 SaltBase, uint32 SaltStep, uint32 Salt2, uint32 Salt3,
    float XYScale, float MinRadius, float MaxRadius,
    FEmit&& Emit)   // Emit(X, Y, Radius, H3)
{
    if (Density <= 0.0f) return;

    for (int32 i = 0; i < MaxCount; i++)
    {
        const uint32 H = VoxelHash::Mix(CR.Hash ^ (SaltBase + (uint32)i * SaltStep));
        if (VoxelHash::ToFloat01(H) > Density) continue;

        const uint32 H2 = VoxelHash::Mix(H ^ Salt2);
        const uint32 H3 = VoxelHash::Mix(H2 ^ Salt3);

        const float X = CR.Center.X + VoxelHash::ToFloatSigned(H2) * CR.RadiusXY * XYScale;
        const float Y = CR.Center.Y + VoxelHash::ToFloatSigned(VoxelHash::Mix(H2)) * CR.RadiusXY * XYScale;
        const float R = FMath::Lerp(MinRadius, MaxRadius, VoxelHash::ToFloat01(H3));

        Emit(X, Y, R, H3);
    }
}

//=============================================================================
// PHASE 1: BUILD CHUNK CACHE
//=============================================================================
// Collects all rooms in the COLLECT region, computes a window-invariant
// nearest-neighbor backbone for connectivity, decides tunnel connections, and
// pre-computes all tunnel geometry (radii, Z offsets, midpoint warping, bounding
// spheres). Only rooms/tunnels relevant to the chunk (STORE region) are kept.
//
// The result is stored in OutCache and reused for every voxel in the chunk.

void VoxelCaveMorphology::BuildChunkCache(
    FChunkSDFCache& OutCache,
    float SearchMinX, float SearchMinY,
    float SearchMaxX, float SearchMaxY,
    const FStrateGenerationParams& Params,
    uint32 Seed, int32 StrateIndex,
    const TArray<FStrateTerrainOpEntry>* TerrainOps)
{
    // Clear previous data (arrays keep their allocation for reuse)
    OutCache.Rooms.Reset();
    OutCache.Tunnels.Reset();
    OutCache.Pits.Reset();
    OutCache.Chimneys.Reset();
    OutCache.Columns.Reset();

    // Combine world seed with strate index so each strate gets unique caves
    const uint32 StrateSeed = VoxelHash::Mix(Seed ^ (uint32)(StrateIndex * 7919 + 104729));

    const float CellSize = Params.RoomSpacing;
    if (CellSize <= 0.0f) return;

    //=========================================================================
    // INFLUENCE RADII
    //=========================================================================
    // MaxInfluence = how far a room body / tunnel TUBE reaches PERPENDICULAR to its
    // anchor — NOT its length. A room or tunnel whose anchor lies within MaxInfluence
    // of a box can touch a voxel inside that box.
    // Envelope conservatif / conservative bound: Lerp accepts inverted endpoints,
    // so max(Min, Max) covers either radius without changing the authored roll.
    const float RoomRadiusEnvelope = FMath::Max(Params.MinRoomRadius, Params.MaxRoomRadius);
    const float TunnelRadiusEnvelope = FMath::Max(Params.TunnelMinRadius, Params.TunnelMaxRadius);
    const float MaxInfluence = FMath::Max(
        RoomRadiusEnvelope,
        Params.TunnelWarpStrength + TunnelRadiusEnvelope
    ) + Params.SDFBlendRadius;

    const float MaxTunnelLen = FMath::Max(Params.MaxTunnelLength, 0.0f);

    //=========================================================================
    // STORE decision — what we keep for the per-voxel loop.
    //=========================================================================
    // A room/tunnel is stored iff its OWN influence sphere can overlap the search box
    // (RoomReachesSearchBox / tunnel bounding spheres below). Per-voxel culling refines.

    //=========================================================================
    // COLLECT region — what we hash into existence for the connectivity decision.
    //=========================================================================
    // To DECIDE the graph identically in neighboring chunks, we must see, for every
    // room that could emit a tunnel touching this chunk, that room's ENTIRE
    // nearest-neighbor candidate set (all rooms within MaxTunnelLength of it):
    //   - A tunnel touching the chunk has BOTH endpoints within
    //     (MaxTunnelLength + MaxInfluence) of the search box (capsule len <= MaxTunnelLength).
    //   - Each endpoint's NN candidates lie within MaxTunnelLength of that endpoint.
    //   => collect within (2 * MaxTunnelLength + MaxInfluence) of the search box.
    // Combined with NN candidates being filtered to <= MaxTunnelLength below, this
    // makes the backbone decision for any STORED tunnel window-invariant.
    const float CollectMargin = 2.0f * MaxTunnelLen + MaxInfluence;
    const float CollectMinX = SearchMinX - CollectMargin;
    const float CollectMinY = SearchMinY - CollectMargin;
    const float CollectMaxX = SearchMaxX + CollectMargin;
    const float CollectMaxY = SearchMaxY + CollectMargin;

    // Convert COLLECT bounds to cell range
    const int32 CellMinX = FMath::FloorToInt(CollectMinX / CellSize);
    const int32 CellMaxX = FMath::FloorToInt(CollectMaxX / CellSize);
    const int32 CellMinY = FMath::FloorToInt(CollectMinY / CellSize);
    const int32 CellMaxY = FMath::FloorToInt(CollectMaxY / CellSize);

    //=========================================================================
    // Vertical range for room CENTER placement.
    //=========================================================================
    // Buffer = seal thickness + max room half-height.
    // This guarantees the tallest possible room (RoomRadiusEnvelope * RoomHeightRatio)
    // fits entirely within the seal boundary — no room gets its ceiling or floor
    // cut flat by the seal. Smaller rooms have proportionally more margin.
    const float RoomZBuffer = RoomRadiusEnvelope * Params.RoomHeightRatio;
    const float StrateMinZ  = Params.StrateBottomWorldZ + Params.BoundarySealThickness + RoomZBuffer;
    const float StrateMaxZ  = Params.StrateTopWorldZ   - Params.BoundarySealThickness - RoomZBuffer;
    const float StrateRangeZ = StrateMaxZ - StrateMinZ;
    const float StrateCenterZ = (StrateMinZ + StrateMaxZ) * 0.5f;

    const float BlendK = Params.SDFBlendRadius;

    // "This room can reach the search box" test, using the room's OWN reach — the same
    // extent formula as its per-voxel cull sphere (1.5x radius for capsule-stretched
    // variants + blend margin). The old test compared the center against a box inflated
    // by the SHARED MaxInfluence, which silently assumed every room's reach <= MaxInfluence.
    // That's FALSE for the origin room (OriginRoomRadius >> MaxRoomRadius) — chunks inside
    // the big room but > MaxInfluence from (0,0) didn't store it, so its carve clipped at an
    // arbitrary chunk-aligned radius — and slightly false even for hash rooms (1.5x stretch).
    auto RoomReachesSearchBox = [&](const FVector& C, float RadiusXY, float RadiusZ) -> bool
    {
        const float Reach = FMath::Max(RadiusXY * 1.5f, RadiusZ) + BlendK * 3.0f;
        const float dx = FMath::Max3((float)(SearchMinX - C.X), 0.0f, (float)(C.X - SearchMaxX));
        const float dy = FMath::Max3((float)(SearchMinY - C.Y), 0.0f, (float)(C.Y - SearchMaxY));
        return (dx * dx + dy * dy) <= Reach * Reach;
    };

    // Sphere-vs-search-box test in XY (treated as infinite in Z; per-voxel culling
    // resolves Z). Used to decide whether a tunnel is worth storing for this chunk.
    auto SphereTouchesSearchXY = [&](const FVector& C, float RSq) -> bool
    {
        const float dx = FMath::Max3((float)(SearchMinX - C.X), 0.0f, (float)(C.X - SearchMaxX));
        const float dy = FMath::Max3((float)(SearchMinY - C.Y), 0.0f, (float)(C.Y - SearchMaxY));
        return (dx * dx + dy * dy) <= RSq;
    };

    // Temporary array with cell coordinates for tunnel connection decisions
    TArray<FBuildRoom, TInlineAllocator<64>> BuildRooms;

    // --- ORIGIN ROOM ---
    // Guaranteed large room at (0, 0) in each strate — the (0,0) descent spine hub.
    // Collected whenever (0,0) is inside the COLLECT region so it participates in the
    // connectivity decision; only stored if it can reach this chunk.
    int32 OriginIdx = -1;
    if (Params.OriginRoomRadius > 0.0f)
    {
        // Collected when (0,0) is in the COLLECT region (connectivity) OR when the room's
        // own body can reach this chunk (store) — its radius may exceed the collect margin.
        const bool bOriginInCollect =
            0.0f >= CollectMinX && 0.0f <= CollectMaxX &&
            0.0f >= CollectMinY && 0.0f <= CollectMaxY;
        const float OriginRZ = Params.OriginRoomRadius * Params.RoomHeightRatio;
        if (bOriginInCollect ||
            RoomReachesSearchBox(FVector(0.0f, 0.0f, StrateCenterZ), Params.OriginRoomRadius, OriginRZ))
        {
            FBuildRoom OriginRoom;
            OriginRoom.CellX = INT32_MAX;  // Sentinel — never matches a real grid cell
            OriginRoom.CellY = INT32_MAX;
            OriginRoom.Hash = VoxelHash::Cell(0, 0, StrateSeed ^ 0x0A161Cu);
            OriginRoom.Center = FVector(0.0f, 0.0f, StrateCenterZ);
            OriginRoom.RadiusXY = Params.OriginRoomRadius;
            OriginRoom.RadiusZ = Params.OriginRoomRadius * Params.RoomHeightRatio;
            OriginRoom.bIsOrigin = true;
            OriginRoom.bStore = RoomReachesSearchBox(OriginRoom.Center, OriginRoom.RadiusXY, OriginRoom.RadiusZ);
            OriginIdx = BuildRooms.Add(OriginRoom);
        }
    }

    // --- HASH-BASED ROOMS ---
    for (int32 CY = CellMinY; CY <= CellMaxY; CY++)
    {
        for (int32 CX = CellMinX; CX <= CellMaxX; CX++)
        {
            // Hash this cell to decide if it has a room
            const uint32 CellHash = VoxelHash::Cell(CX, CY, StrateSeed);
            const float RoomChance = VoxelHash::ToFloat01(CellHash);

            // Skip empty cells (no room here)
            if (RoomChance >= Params.RoomDensity) continue;

            // Room position: jittered within the cell
            const float JitterX = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x12345678u));
            const float JitterY = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x9ABCDEF0u));
            const float JitterZ = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x55AA55AAu));

            FBuildRoom Room;
            Room.CellX = CX;
            Room.CellY = CY;
            Room.Hash = CellHash;
            Room.Center.X = (CX + 0.15f + JitterX * 0.7f) * CellSize;
            Room.Center.Y = (CY + 0.15f + JitterY * 0.7f) * CellSize;
            Room.Center.Z = StrateMinZ + JitterZ * FMath::Max(StrateRangeZ, 1.0f);

            // Room size: lerp between min and max
            const float SizeFactor = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0xFEDCBA98u));
            Room.RadiusXY = FMath::Lerp(Params.MinRoomRadius, Params.MaxRoomRadius, SizeFactor);
            Room.RadiusZ = Room.RadiusXY * Params.RoomHeightRatio;
            Room.bIsOrigin = false;
            Room.bStore = RoomReachesSearchBox(Room.Center, Room.RadiusXY, Room.RadiusZ);

            BuildRooms.Add(Room);
        }
    }

    const int32 NumRooms = BuildRooms.Num();
    if (NumRooms == 0) return;

    //=========================================================================
    // Window-invariant guaranteed backbone
    //=========================================================================
    // Each room gets ONE guaranteed link, chosen among candidates within MaxTunnelLength
    // (that reach filter is what keeps the decision identical across chunk windows).
    //
    // bTunnelsFlowTowardOrigin = true (default): the link target is the best candidate
    // among rooms STRICTLY CLOSER to (0,0) in XY. Every chain of links then descends in
    // origin-distance and terminates at the origin room → the network is a TREE ROOTED AT
    // THE SPINE HUB: every room is reachable, tunnels flow inward like tributaries.
    // (Frontier rooms with no closer candidate in reach fall back to plain NN — a far
    // cluster stays internally chained even when it can't bridge to the origin side.)
    //
    // bTunnelsFlowTowardOrigin = false (legacy): plain nearest-neighbor pairing. NOTE:
    // despite what this comment used to claim, an NN-graph is a FOREST of small clusters,
    // not a connected tree — isolated cave pockets are expected in this mode.
    //
    // Selection metric (not the reach filter) penalizes vertical separation via
    // TunnelHorizontalBias, so the GUARANTEED links also prefer walkable slopes —
    // previously only the random TunnelDensity extras were biased, which is why
    // backbone tunnels could come out absurdly steep.
    TArray<int32, TInlineAllocator<64>> NearestNeighbor;
    NearestNeighbor.SetNumUninitialized(NumRooms);

    const float MaxTunnelLenSq = MaxTunnelLen * MaxTunnelLen;
    const bool bFlowToOrigin = Params.bTunnelsFlowTowardOrigin;

    auto LinkMetric = [&](int32 I, int32 J) -> float
    {
        const float D = FVector::Dist(BuildRooms[I].Center, BuildRooms[J].Center);
        const float VertSep = FMath::Abs(BuildRooms[I].Center.Z - BuildRooms[J].Center.Z);
        return D + VertSep * Params.TunnelHorizontalBias * 5.0f;
    };
    // Squared XY distance to the (0,0) spine — the "inward" ordering. Purely positional,
    // so it is window-invariant by construction.
    auto OriginKeySq = [&](int32 I) -> float
    {
        const FVector& C = BuildRooms[I].Center;
        return C.X * C.X + C.Y * C.Y;
    };

    // ExcludeJ: used by the origin-cap redirect below (re-pick ignoring the origin room).
    auto PickNeighbor = [&](int32 I, int32 ExcludeJ) -> int32
    {
        const float MyKeySq = OriginKeySq(I);
        float BestInward = FLT_MAX;  int32 BestInwardJ = -1;
        float BestAny    = FLT_MAX;  int32 BestAnyJ    = -1;
        for (int32 J = 0; J < NumRooms; J++)
        {
            if (J == I || J == ExcludeJ) continue;
            const float DSq = FVector::DistSquared(BuildRooms[I].Center, BuildRooms[J].Center);
            if (DSq > MaxTunnelLenSq) continue;       // out of reach — never a tunnel
            const float M = LinkMetric(I, J);
            if (M < BestAny) { BestAny = M; BestAnyJ = J; }
            if (bFlowToOrigin && OriginKeySq(J) < MyKeySq && M < BestInward)
            {
                BestInward = M; BestInwardJ = J;
            }
        }
        return (bFlowToOrigin && BestInwardJ != -1) ? BestInwardJ : BestAnyJ;
    };

    for (int32 I = 0; I < NumRooms; I++)
    {
        NearestNeighbor[I] = PickNeighbor(I, /*ExcludeJ=*/INDEX_NONE);
    }

    //=========================================================================
    // ORIGIN CONNECTION CAP (deterministic, order-independent)
    //=========================================================================
    // OriginRoomMaxConnections caps how many rooms backbone-force into the origin.
    // The OLD approach counted connections in pair-loop order, which depended on the
    // per-chunk room ordering → non-deterministic across chunks. Instead we gather
    // ALL rooms backbone-linked to origin (window-invariant given the COLLECT region),
    // rank them by a deterministic pair hash, and keep only the top N as forced.
    // The rest are DOWNGRADED to the random TunnelDensity path (connectivity not
    // broken, only the "guaranteed" aspect is limited).
    TSet<int32> OriginDowngraded;
    const int32 MaxOriginConn = Params.OriginRoomMaxConnections;
    if (OriginIdx >= 0 && MaxOriginConn > 0)
    {
        // Collect origin backbone candidates with a deterministic ranking key.
        TArray<TPair<uint32, int32>, TInlineAllocator<32>> OriginLinks;
        for (int32 I = 0; I < NumRooms; I++)
        {
            if (I == OriginIdx) continue;
            const bool bLinked = (NearestNeighbor[I] == OriginIdx) || (NearestNeighbor[OriginIdx] == I);
            if (!bLinked) continue;
            const uint32 Key = VoxelHash::Pair(
                BuildRooms[OriginIdx].CellX, BuildRooms[OriginIdx].CellY,
                BuildRooms[I].CellX, BuildRooms[I].CellY,
                StrateSeed ^ 0x031A1Eu);
            OriginLinks.Add(TPair<uint32, int32>(Key, I));
        }
        // Stable deterministic order by (hash, then index for tie-break).
        OriginLinks.Sort([](const TPair<uint32, int32>& A, const TPair<uint32, int32>& B)
        {
            return A.Key != B.Key ? A.Key < B.Key : A.Value < B.Value;
        });
        for (int32 R = MaxOriginConn; R < OriginLinks.Num(); ++R)
        {
            OriginDowngraded.Add(OriginLinks[R].Value);
        }

        // REDIRECT, don't strand: a downgraded room whose guaranteed link pointed at the
        // origin re-picks its best target EXCLUDING origin. It keeps a guaranteed link
        // (chains to the hub through another room instead of directly), which matters
        // doubly now that rooms with zero connections are culled below. Deterministic:
        // same candidate set, same metric, one exclusion.
        for (int32 DowngradedI : OriginDowngraded)
        {
            if (NearestNeighbor[DowngradedI] == OriginIdx)
            {
                NearestNeighbor[DowngradedI] = PickNeighbor(DowngradedI, /*ExcludeJ=*/OriginIdx);
            }
        }
    }

    //=========================================================================
    // Resolve tunnel connections, pre-compute geometry, store chunk-relevant ones
    //=========================================================================
    // A tunnel exists if EITHER:
    //   1. One is the other's nearest neighbor (backbone — guarantees connectivity),
    //      and (for origin links) it survived the origin cap, OR
    //   2. The pair hash passes TunnelDensity (random extra loops).
    // Both must pass the distance check (MaxTunnelLength). Only tunnels whose bounding
    // sphere reaches the search box are stored for the per-voxel loop.
    // Tracks whether each room ends up with at least one tunnel — DECIDED connections,
    // independent of whether the tunnel itself is stored for this chunk (a room near the
    // window edge may have all its tunnels outside the box; it's still "connected").
    // Stored rooms with zero connections are culled at emission: they'd be sealed air
    // pockets no tunnel ever reaches. Window-invariant: a stored room's full candidate
    // set (and each candidate's own candidates) lies inside the COLLECT region.
    TArray<bool, TInlineAllocator<64>> RoomConnected;
    RoomConnected.Init(false, NumRooms);

    for (int32 I = 0; I < NumRooms; I++)
    {
        for (int32 J = I + 1; J < NumRooms; J++)
        {
            const FBuildRoom& RoomA = BuildRooms[I];
            const FBuildRoom& RoomB = BuildRooms[J];
            bool bBackbone = (NearestNeighbor[I] == J) || (NearestNeighbor[J] == I);

            // Origin cap: downgrade backbone links beyond the deterministic top-N.
            if (bBackbone && (RoomA.bIsOrigin || RoomB.bIsOrigin))
            {
                const int32 Other = RoomA.bIsOrigin ? J : I;
                if (OriginDowngraded.Contains(Other))
                {
                    bBackbone = false;  // Let TunnelDensity decide instead
                }
            }

            // --- DISTANCE CHECK ---
            const float EuclidDist = FVector::Dist(RoomA.Center, RoomB.Center);
            float CheckDist = EuclidDist;

            // Horizontal bias: penalize vertical separation for non-backbone tunnels
            if (!bBackbone && Params.TunnelHorizontalBias > 0.0f)
            {
                const float VertSep = FMath::Abs(RoomA.Center.Z - RoomB.Center.Z);
                CheckDist += VertSep * Params.TunnelHorizontalBias * 5.0f;
            }

            if (CheckDist > Params.MaxTunnelLength) continue;

            // --- CONNECTION DECISION ---
            if (!bBackbone)
            {
                const uint32 PairHash = VoxelHash::Pair(
                    RoomA.CellX, RoomA.CellY,
                    RoomB.CellX, RoomB.CellY,
                    StrateSeed
                );
                const float ConnectChance = VoxelHash::ToFloat01(PairHash);
                if (ConnectChance >= Params.TunnelDensity) continue;
            }

            // Connection DECIDED (backbone or density roll) — both rooms are reachable.
            RoomConnected[I] = true;
            RoomConnected[J] = true;

            // --- TUNNEL HASH (for deriving all tunnel properties) ---
            const uint32 TunnelHash = VoxelHash::Pair(
                RoomA.CellX, RoomA.CellY,
                RoomB.CellX, RoomB.CellY,
                StrateSeed ^ 0xDECAF001u
            );

            // --- RADIUS ---
            const float FactorA = VoxelHash::ToFloat01(VoxelHash::Mix(TunnelHash ^ 0xBAADF00Du));
            const float FactorB = VoxelHash::ToFloat01(VoxelHash::Mix(TunnelHash ^ 0x8BADF00Du));
            const float RadA = FMath::Lerp(Params.TunnelMinRadius, Params.TunnelMaxRadius, FactorA);
            const float RadB = FMath::Lerp(Params.TunnelMinRadius, Params.TunnelMaxRadius, FactorB);

            // --- ENDPOINT Z OFFSET ---
            const float ZOffsetA = VoxelHash::ToFloatSigned(VoxelHash::Mix(TunnelHash ^ 0xA1B2C3D4u))
                * RoomA.RadiusZ * Params.TunnelEndpointZOffset;
            const float ZOffsetB = VoxelHash::ToFloatSigned(VoxelHash::Mix(TunnelHash ^ 0xD4C3B2A1u))
                * RoomB.RadiusZ * Params.TunnelEndpointZOffset;

            FVector EndA = RoomA.Center + FVector(0.0f, 0.0f, ZOffsetA);
            FVector EndB = RoomB.Center + FVector(0.0f, 0.0f, ZOffsetB);

            // --- BUILD CACHED TUNNEL ---
            FCachedTunnel CT;
            CT.EndpointA = EndA;
            CT.EndpointB = EndB;
            CT.RadiusA = RadA;
            CT.RadiusB = RadB;

            // --- PATH WARPING ---
            const float TunnelLength = FVector::Dist(EndA, EndB);

            if (Params.TunnelWarpStrength > 0.0f && TunnelLength > 1.0f)
            {
                FVector TunnelDir = (EndB - EndA).GetSafeNormal();
                FVector PerpH = FVector(-TunnelDir.Y, TunnelDir.X, 0.0f);

                float MaxWarp = FMath::Min(Params.TunnelWarpStrength, TunnelLength * 0.25f);
                float WarpH = VoxelHash::ToFloatSigned(VoxelHash::Mix(TunnelHash ^ 0x1234ABCDu)) * MaxWarp;
                float WarpV = VoxelHash::ToFloatSigned(VoxelHash::Mix(TunnelHash ^ 0x5678EF01u)) * MaxWarp * 0.3f;

                FVector Mid = (EndA + EndB) * 0.5f;
                Mid += PerpH * WarpH + FVector(0.0f, 0.0f, WarpV);

                CT.Midpoint = Mid;
                CT.RadiusMid = (RadA + RadB) * 0.5f;
                CT.bHasMidpoint = true;

                // Bounding sphere: encloses all 3 control points + max radius
                CT.BoundCenter = (EndA + Mid + EndB) / 3.0f;
                float MaxR = FMath::Max3(RadA, RadB, CT.RadiusMid) + BlendK;
                float DistA = FVector::Dist(CT.BoundCenter, EndA);
                float DistM = FVector::Dist(CT.BoundCenter, Mid);
                float DistB = FVector::Dist(CT.BoundCenter, EndB);
                float BoundR = FMath::Max3(DistA, DistM, DistB) + MaxR;
                CT.BoundRadiusSq = BoundR * BoundR;
            }
            else
            {
                CT.Midpoint = FVector::ZeroVector;
                CT.RadiusMid = 0.0f;
                CT.bHasMidpoint = false;

                // Bounding sphere: encloses both endpoints + max radius
                CT.BoundCenter = (EndA + EndB) * 0.5f;
                float MaxR = FMath::Max(RadA, RadB) + BlendK;
                float HalfLen = TunnelLength * 0.5f;
                float BoundR = HalfLen + MaxR;
                CT.BoundRadiusSq = BoundR * BoundR;
            }

            // Store only if this tunnel can actually reach a voxel in this chunk.
            if (SphereTouchesSearchXY(CT.BoundCenter, CT.BoundRadiusSq))
            {
                OutCache.Tunnels.Add(CT);
            }
        }
    }

    //=========================================================================
    // Copy STORE-relevant rooms to cache (cull radii + per-room terrain ops),
    // then pre-bake their pits / chimneys / columns.
    //=========================================================================
    // Build terrain op pool stats once (outside the per-room loop).
    // TerrainOps is the strate's probability pool; each entry has a Probability
    // in [0,1]. We do a weighted random draw per room using the room's hash.
    //
    // PROBABILITY MODEL:
    //   - Entries are checked cumulatively (like drawing from a bucket).
    //   - The "no op" slot takes the remaining probability space (if sum < 1.0).
    //   - If sum >= 1.0, every room gets an op (normalized selection).
    float TotalOpProb = 0.0f;
    if (TerrainOps)
    {
        for (const FStrateTerrainOpEntry& E : *TerrainOps)
            TotalOpProb += FMath::Max(E.Probability, 0.0f);
    }
    const float NormFactor = (TotalOpProb > 1.0f) ? (1.0f / TotalOpProb) : 1.0f;

    // Temporary params struct for reading op fields (PitDensity, PitMinRadius, etc.)
    FStrateGenerationParams OpParams;

    OutCache.Rooms.Reserve(NumRooms);

    for (int32 RoomIdx = 0; RoomIdx < NumRooms; RoomIdx++)
    {
        const FBuildRoom& BR = BuildRooms[RoomIdx];
        if (!BR.bStore) continue;  // Far room — collected for connectivity only

        // Sealed-bubble cull: a room no tunnel ever reaches would be an isolated air
        // pocket — don't carve it at all. The origin room is always kept (spine hub).
        if (!RoomConnected[RoomIdx] && !BR.bIsOrigin) continue;

        FCachedRoom CR;
        CR.Center = BR.Center;
        CR.RadiusXY = BR.RadiusXY;
        CR.RadiusZ = BR.RadiusZ;
        CR.Hash = BR.Hash;
        CR.bIsOrigin = BR.bIsOrigin;

        // Cull radius: max extent the room can reach + blend margin.
        // 1.5x accounts for capsule shapes extending beyond nominal radius.
        float MaxExtent = FMath::Max(BR.RadiusXY * 1.5f, BR.RadiusZ) + BlendK * 3.0f;
        CR.CullRadiusSq = MaxExtent * MaxExtent;

        // --- PRE-BAKED SHAPE ---
        // Same hash roll + thresholds + capsule trig the evaluator used to redo PER VOXEL;
        // done once here → EvaluateSDFCached just switches on ShapeType. Bit-identical output.
        {
            const uint32 ShapeHash = VoxelHash::Mix(CR.Hash ^ 0xDEADBEEFu);
            const float ShapeRoll = CR.bIsOrigin ? 0.0f : VoxelHash::ToFloat01(ShapeHash);
            const float BoxThreshold     = 1.0f - Params.RoomShapeVariety * 0.5f;
            const float CapsuleThreshold = 1.0f - Params.RoomShapeVariety * 0.2f;

            if (ShapeRoll >= BoxThreshold && ShapeRoll < CapsuleThreshold)
            {
                // ROUNDED BOX: angular chamber with smooth corners
                CR.ShapeType = 1;
                CR.ShapeA = FVector(CR.RadiusXY * 0.8f, CR.RadiusXY * 0.8f, CR.RadiusZ * 0.8f);
                CR.ShapeR = CR.RadiusXY * 0.25f;
            }
            else if (ShapeRoll >= CapsuleThreshold)
            {
                // ELONGATED CAPSULE: stretched hall/corridor-room
                CR.ShapeType = 2;
                const float DirAngle = VoxelHash::ToFloat01(VoxelHash::Mix(CR.Hash ^ 0xCAFEBABEu)) * 2.0f * PI;
                const float StretchDist = CR.RadiusXY * 0.7f;
                const FVector Dir(FMath::Cos(DirAngle), FMath::Sin(DirAngle), 0.0f);
                CR.ShapeA = CR.Center + Dir * StretchDist;
                CR.ShapeB = CR.Center - Dir * StretchDist;
                CR.ShapeR = FMath::Min(CR.RadiusXY * 0.6f, CR.RadiusZ);
            }
            else
            {
                // ELLIPSOID (default): smooth oval chamber
                CR.ShapeType = 0;
                CR.ShapeA = FVector(CR.RadiusXY, CR.RadiusXY, CR.RadiusZ);
            }
        }

        // Flat floor cut: soft floor plane per room, hash-rolled from [Min, Max].
        // SmoothMax applied in EvaluateSDFCached so tunnels/pits don't create hard seams.
        // Sentinel -FLT_MAX means "no cut" so the per-voxel check is a single compare.
        {
            const float FloorRoll = VoxelHash::ToFloat01(VoxelHash::Mix(BR.Hash ^ 0xF100F2u));
            const float FloorCut  = FMath::Lerp(
                FMath::Min(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
                FMath::Max(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
                FloorRoll
            );

            if (FloorCut < 1.0f)
            {
                CR.FloorCutZ            = CR.Center.Z - CR.RadiusZ * FloorCut;
                CR.FloorReliefStrength  = Params.FloorReliefStrength;
                CR.FloorReliefFrequency = Params.FloorReliefFrequency;
                CR.FloorSeed            = VoxelHash::Mix(BR.Hash ^ 0xF100F1u);
            }
            else
            {
                CR.FloorCutZ            = -FLT_MAX;
                CR.FloorReliefStrength  = 0.0f;
                CR.FloorReliefFrequency = 0.015f;
                CR.FloorSeed            = 0;
            }
        }

        // --- PER-ROOM TERRAIN OP SELECTION ---
        if (TerrainOps && TerrainOps->Num() > 0 && TotalOpProb > 0.0f)
        {
            const uint32 OpHash = VoxelHash::Mix(BR.Hash ^ 0x0FEED00u);
            const float Roll = VoxelHash::ToFloat01(OpHash);

            float Cursor = 0.0f;
            for (const FStrateTerrainOpEntry& E : *TerrainOps)
            {
                Cursor += FMath::Max(E.Probability, 0.0f) * NormFactor;
                if (Roll < Cursor)
                {
                    const UVoxelTerrainOpDefinition* Op = E.Operation.Get();
                    if (Op)
                    {
                        CR.RoomOp = Op;
                        CR.RoomOpWeight = E.Weight;
                    }
                    break;
                }
            }
        }

        OutCache.Rooms.Add(CR);

        //---------------------------------------------------------------------
        // PRE-BAKE: PITS, CHIMNEYS, COLUMNS for this room
        //---------------------------------------------------------------------
        // Pre-baking makes features independent of NearestRoomIdx (no thin "lid"
        // when the owning room flips mid-shaft). Only stored rooms are baked —
        // a far room's features can't reach this chunk anyway.
        if (!CR.RoomOp) continue;

        OpParams = FStrateGenerationParams{};
        CR.RoomOp->ApplyTo(OpParams, CR.RoomOpWeight);

        // PITS — downward shafts anchored in the room's lower half.
        BakeRoomFeature(CR, /*Max*/2, OpParams.PitDensity,
            0xDE1A7Eu, 6271u, 0xABCDu, 0x5EEDu,
            /*XYScale*/0.6f, OpParams.PitMinRadius, OpParams.PitMaxRadius,
            [&](float PX, float PY, float PitRadius, uint32 PH3)
            {
                const uint32 PH4 = VoxelHash::Mix(PH3 ^ 0xF00Du);
                FCachedPit Pit;
                Pit.CenterX       = PX;
                Pit.CenterY       = PY;
                Pit.TopZ          = CR.Center.Z - CR.RadiusZ * 0.5f
                                    + VoxelHash::ToFloat01(PH4) * CR.RadiusZ * 0.2f;
                Pit.Radius        = PitRadius;
                Pit.Depth         = OpParams.PitDepth;
                Pit.FlareDist     = PitRadius * 2.0f;
                Pit.FlareExtra    = PitRadius * 1.0f;
                Pit.BaseDensity   = Params.BaseDensity;
                Pit.BlendK        = Params.SDFBlendRadius;
                const float MaxXYR = PitRadius + PitRadius + Params.SDFBlendRadius + 4.0f;
                Pit.BoundXYRadiusSq = MaxXYR * MaxXYR;
                OutCache.Pits.Add(Pit);
            });

        // CHIMNEYS — mirror of pits: upward tubes anchored in the room's upper half.
        BakeRoomFeature(CR, /*Max*/2, OpParams.ChimneyDensity,
            0xC4F007u, 7919u, 0x1337u, 0xCAFEu,
            /*XYScale*/0.6f, OpParams.ChimneyMinRadius, OpParams.ChimneyMaxRadius,
            [&](float CX, float CY, float ChmRadius, uint32 CH3)
            {
                const uint32 CH4 = VoxelHash::Mix(CH3 ^ 0xD00Du);
                FCachedChimney Chim;
                Chim.CenterX       = CX;
                Chim.CenterY       = CY;
                Chim.BottomZ       = CR.Center.Z + CR.RadiusZ * 0.5f
                                     - VoxelHash::ToFloat01(CH4) * CR.RadiusZ * 0.2f;
                Chim.Radius        = ChmRadius;
                Chim.Height        = OpParams.ChimneyHeight;
                Chim.FlareDist     = ChmRadius * 2.0f;
                Chim.FlareExtra    = ChmRadius * 1.0f;
                Chim.BaseDensity   = Params.BaseDensity;
                Chim.BlendK        = Params.SDFBlendRadius;
                const float MaxXYR = ChmRadius + ChmRadius + Params.SDFBlendRadius + 4.0f;
                Chim.BoundXYRadiusSq = MaxXYR * MaxXYR;
                OutCache.Chimneys.Add(Chim);
            });

        // COLUMNS — full-height solid cylinders (no Z anchor, no flare).
        BakeRoomFeature(CR, /*Max*/4, OpParams.ColumnDensity,
            0xC01C01u, 3571u, 0x1A2B3Cu, 0xBEEFu,
            /*XYScale*/0.75f, OpParams.ColumnMinRadius, OpParams.ColumnMaxRadius,
            [&](float ColX, float ColY, float ColR, uint32 /*H3*/)
            {
                FCachedColumn Col;
                Col.CenterX       = ColX;
                Col.CenterY       = ColY;
                Col.Radius        = ColR;
                Col.BaseDensity   = Params.BaseDensity;
                const float MaxXYR = ColR + 6.0f;
                Col.BoundXYRadiusSq = MaxXYR * MaxXYR;
                OutCache.Columns.Add(Col);
            });
    }
}

//=============================================================================
// PHASE 2: EVALUATE SDF WITH CACHED DATA
//=============================================================================
// Pure SDF math — no hashing, no array building, no backbone computation.
// Just loops through the pre-built rooms and tunnels, evaluates distance,
// and smooth-mins everything together. Distance culling skips primitives
// that are clearly too far to contribute.

float VoxelCaveMorphology::EvaluateSDFCached(
    float WorldX, float WorldY, float WorldZ,
    const FChunkSDFCache& Cache,
    float SDFBlendRadius,
    int32* OutNearestRoomIdx)
{
    float MinSDF = FLT_MAX;
    const float BlendK = SDFBlendRadius;
    const FVector Pos(WorldX, WorldY, WorldZ);

    // Track which room contributes the smallest (most-inside) raw SDF.
    // This is used by the terrain ops system to find the "owning" room for
    // this voxel and apply that room's per-room terrain operation.
    // We track raw room SDF (before SmoothMin) so tunnel SDFs don't interfere.
    float NearestRoomRawSDF = FLT_MAX;
    int32 NearestIdx = -1;

    //=========================================================================
    // Room SDFs
    //=========================================================================
    for (int32 RoomIdx = 0; RoomIdx < Cache.Rooms.Num(); ++RoomIdx)
    {
        const FCachedRoom& Room = Cache.Rooms[RoomIdx];

        // --- DISTANCE CULL ---
        const float DistSq = FVector::DistSquared(Pos, Room.Center);
        if (DistSq > Room.CullRadiusSq) continue;

        // --- SHAPE (pre-baked in BuildChunkCache — no per-voxel hash roll / trig) ---
        float RoomSDF;
        switch (Room.ShapeType)
        {
        case 1:  RoomSDF = VoxelSDF::RoundedBox(Pos, Room.Center, Room.ShapeA, Room.ShapeR); break;
        case 2:  RoomSDF = VoxelSDF::Capsule(Pos, Room.ShapeA, Room.ShapeB, Room.ShapeR);    break;
        default: RoomSDF = VoxelSDF::Ellipsoid(Pos, Room.Center, Room.ShapeA);               break;
        }

        // Soft floor: SmoothMax of the room SDF and the floor half-space.
        if (Room.FloorCutZ > -FLT_MAX)
        {
            float FloorZ = Room.FloorCutZ;

            if (Room.FloorReliefStrength > 0.0f)
            {
                const float RF = Room.FloorReliefFrequency;
                const float SF = (float)Room.FloorSeed * 0.00001f;

                float N = FMath::PerlinNoise2D(FVector2D(Pos.X * RF + SF,       Pos.Y * RF + SF * 1.7f)) * 0.65f
                        + FMath::PerlinNoise2D(FVector2D(Pos.X * RF * 2.3f + SF * 3.1f, Pos.Y * RF * 2.3f + SF * 5.3f)) * 0.35f;
                N *= VOXEL_NOISE_SCALE;
                FloorZ += N * Room.FloorReliefStrength;
            }

            RoomSDF = VoxelSDF::SmoothMax(RoomSDF, FloorZ - Pos.Z, BlendK * 0.35f);
        }

        // Track the room whose SDF is smallest (most inside).
        if (OutNearestRoomIdx && RoomSDF < NearestRoomRawSDF)
        {
            NearestRoomRawSDF = RoomSDF;
            NearestIdx = RoomIdx;
        }

        MinSDF = VoxelSDF::SmoothMin(MinSDF, RoomSDF, BlendK);
    }

    //=========================================================================
    // Tunnel SDFs
    //=========================================================================
    for (const FCachedTunnel& Tunnel : Cache.Tunnels)
    {
        // --- BOUNDING SPHERE CULL ---
        const float DistSq = FVector::DistSquared(Pos, Tunnel.BoundCenter);
        if (DistSq > Tunnel.BoundRadiusSq) continue;

        float TunnelSDF;

        if (Tunnel.bHasMidpoint)
        {
            // Two-segment curved tunnel: A→Mid and Mid→B
            float SegA = VoxelSDF::TaperedCapsule(Pos, Tunnel.EndpointA, Tunnel.Midpoint,
                                                   Tunnel.RadiusA, Tunnel.RadiusMid);
            float SegB = VoxelSDF::TaperedCapsule(Pos, Tunnel.Midpoint, Tunnel.EndpointB,
                                                   Tunnel.RadiusMid, Tunnel.RadiusB);
            TunnelSDF = FMath::Min(SegA, SegB);
        }
        else
        {
            // Straight single-segment tunnel
            TunnelSDF = VoxelSDF::TaperedCapsule(Pos, Tunnel.EndpointA, Tunnel.EndpointB,
                                                  Tunnel.RadiusA, Tunnel.RadiusB);
        }

        MinSDF = VoxelSDF::SmoothMin(MinSDF, TunnelSDF, BlendK);
    }

    // Write nearest room index for the caller (terrain ops system)
    if (OutNearestRoomIdx)
    {
        *OutNearestRoomIdx = NearestIdx;
    }

    return MinSDF;
}

//=============================================================================
// CONVENIENCE WRAPPER (backward compatible)
//=============================================================================
// Builds a temporary cache for a single point, then evaluates.
// For chunk generation, use BuildChunkCache + EvaluateSDFCached directly.
// The search box around the point only needs a MaxInfluence margin — BuildChunkCache
// internally widens the COLLECT region to (2*MaxTunnelLength + MaxInfluence) so the
// graph it builds is the same one the chunk path would build at this point.

float VoxelCaveMorphology::EvaluateSDF(
    float WorldX, float WorldY, float WorldZ,
    const FStrateGenerationParams& Params,
    uint32 Seed, int32 StrateIndex)
{
    const float RoomRadiusEnvelope = FMath::Max(Params.MinRoomRadius, Params.MaxRoomRadius);
    const float TunnelRadiusEnvelope = FMath::Max(Params.TunnelMinRadius, Params.TunnelMaxRadius);
    const float Margin = FMath::Max(
        RoomRadiusEnvelope,
        Params.TunnelWarpStrength + TunnelRadiusEnvelope
    ) + Params.SDFBlendRadius;

    FChunkSDFCache TempCache;
    BuildChunkCache(
        TempCache,
        WorldX - Margin, WorldY - Margin,
        WorldX + Margin, WorldY + Margin,
        Params, Seed, StrateIndex
    );

    return EvaluateSDFCached(
        WorldX, WorldY, WorldZ,
        TempCache, Params.SDFBlendRadius
    );
}
