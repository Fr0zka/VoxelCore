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
    const float MaxInfluence = FMath::Max(
        Params.MaxRoomRadius,
        Params.TunnelWarpStrength + Params.TunnelMaxRadius
    ) + Params.SDFBlendRadius;

    const float MaxTunnelLen = FMath::Max(Params.MaxTunnelLength, 0.0f);

    //=========================================================================
    // STORE region — what we keep for the per-voxel loop.
    //=========================================================================
    // A room/tunnel whose influence overlaps the search box is RELEVANT to this
    // chunk. STORE box = search box + MaxInfluence. (Per-voxel culling refines this.)
    const float StoreMinX = SearchMinX - MaxInfluence;
    const float StoreMinY = SearchMinY - MaxInfluence;
    const float StoreMaxX = SearchMaxX + MaxInfluence;
    const float StoreMaxY = SearchMaxY + MaxInfluence;

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
    // This guarantees the tallest possible room (MaxRoomRadius * RoomHeightRatio)
    // fits entirely within the seal boundary — no room gets its ceiling or floor
    // cut flat by the seal. Smaller rooms have proportionally more margin.
    const float RoomZBuffer = Params.MaxRoomRadius * Params.RoomHeightRatio;
    const float StrateMinZ  = Params.StrateBottomWorldZ + Params.BoundarySealThickness + RoomZBuffer;
    const float StrateMaxZ  = Params.StrateTopWorldZ   - Params.BoundarySealThickness - RoomZBuffer;
    const float StrateRangeZ = StrateMaxZ - StrateMinZ;
    const float StrateCenterZ = (StrateMinZ + StrateMaxZ) * 0.5f;

    const float BlendK = Params.SDFBlendRadius;

    // Conservative "this room can reach the STORE box" test. A room's cull sphere
    // radius is <= MaxInfluence, and STORE box already includes a MaxInfluence
    // margin, so testing the center against the STORE box is a correct superset.
    auto CenterInStoreBox = [&](const FVector& C) -> bool
    {
        return C.X >= StoreMinX && C.X <= StoreMaxX
            && C.Y >= StoreMinY && C.Y <= StoreMaxY;
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
        if (0.0f >= CollectMinX && 0.0f <= CollectMaxX &&
            0.0f >= CollectMinY && 0.0f <= CollectMaxY)
        {
            FBuildRoom OriginRoom;
            OriginRoom.CellX = INT32_MAX;  // Sentinel — never matches a real grid cell
            OriginRoom.CellY = INT32_MAX;
            OriginRoom.Hash = VoxelHash::Cell(0, 0, StrateSeed ^ 0x0A161Cu);
            OriginRoom.Center = FVector(0.0f, 0.0f, StrateCenterZ);
            OriginRoom.RadiusXY = Params.OriginRoomRadius;
            OriginRoom.RadiusZ = Params.OriginRoomRadius * Params.RoomHeightRatio;
            OriginRoom.bIsOrigin = true;
            OriginRoom.bStore = CenterInStoreBox(OriginRoom.Center);
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
            Room.bStore = CenterInStoreBox(Room.Center);

            BuildRooms.Add(Room);
        }
    }

    const int32 NumRooms = BuildRooms.Num();
    if (NumRooms == 0) return;

    //=========================================================================
    // Window-invariant nearest-neighbor backbone
    //=========================================================================
    // Each room's nearest neighbor (among rooms within MaxTunnelLength) is a
    // GUARANTEED tunnel connection. Filtering candidates to <= MaxTunnelLength is
    // what makes the result identical across chunks: rooms beyond MaxTunnelLength
    // can never be a tunnel anyway, so excluding them removes the only source of
    // window dependence. This builds a connected tree backbone; TunnelDensity adds
    // loops on top.
    TArray<int32, TInlineAllocator<64>> NearestNeighbor;
    NearestNeighbor.SetNumUninitialized(NumRooms);

    const float MaxTunnelLenSq = MaxTunnelLen * MaxTunnelLen;

    for (int32 I = 0; I < NumRooms; I++)
    {
        float BestDistSq = FLT_MAX;
        int32 BestJ = -1;
        for (int32 J = 0; J < NumRooms; J++)
        {
            if (I == J) continue;
            const float DSq = FVector::DistSquared(BuildRooms[I].Center, BuildRooms[J].Center);
            if (DSq > MaxTunnelLenSq) continue;       // out of reach — never a tunnel
            if (DSq < BestDistSq)
            {
                BestDistSq = DSq;
                BestJ = J;
            }
        }
        NearestNeighbor[I] = BestJ;
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

    for (const FBuildRoom& BR : BuildRooms)
    {
        if (!BR.bStore) continue;  // Far room — collected for connectivity only

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

        // PITS
        if (OpParams.PitDensity > 0.0f)
        {
            const int32 MaxPits = 2;
            for (int32 i = 0; i < MaxPits; i++)
            {
                uint32 PH  = VoxelHash::Mix(BR.Hash ^ (0xDE1A7Eu + (uint32)i * 6271u));
                if (VoxelHash::ToFloat01(PH) > OpParams.PitDensity) continue;

                uint32 PH2 = VoxelHash::Mix(PH  ^ 0xABCDu);
                uint32 PH3 = VoxelHash::Mix(PH2 ^ 0x5EEDu);
                uint32 PH4 = VoxelHash::Mix(PH3 ^ 0xF00Du);

                float PX = CR.Center.X + VoxelHash::ToFloatSigned(PH2) * CR.RadiusXY * 0.6f;
                float PY = CR.Center.Y + VoxelHash::ToFloatSigned(VoxelHash::Mix(PH2)) * CR.RadiusXY * 0.6f;

                float PitRadius = FMath::Lerp(OpParams.PitMinRadius, OpParams.PitMaxRadius,
                                              VoxelHash::ToFloat01(PH3));

                float PitTopZ = CR.Center.Z - CR.RadiusZ * 0.5f
                                + VoxelHash::ToFloat01(PH4) * CR.RadiusZ * 0.2f;

                FCachedPit Pit;
                Pit.CenterX       = PX;
                Pit.CenterY       = PY;
                Pit.TopZ          = PitTopZ;
                Pit.Radius        = PitRadius;
                Pit.Depth         = OpParams.PitDepth;
                Pit.FlareDist     = PitRadius * 2.0f;
                Pit.FlareExtra    = PitRadius * 1.0f;
                Pit.BaseDensity   = Params.BaseDensity;
                Pit.BlendK        = Params.SDFBlendRadius;
                float MaxXYR      = PitRadius + PitRadius + Params.SDFBlendRadius + 4.0f;
                Pit.BoundXYRadiusSq = MaxXYR * MaxXYR;
                OutCache.Pits.Add(Pit);
            }
        }

        // CHIMNEYS
        if (OpParams.ChimneyDensity > 0.0f)
        {
            const int32 MaxChimneys = 2;
            for (int32 i = 0; i < MaxChimneys; i++)
            {
                uint32 CH  = VoxelHash::Mix(BR.Hash ^ (0xC4F007u + (uint32)i * 7919u));
                if (VoxelHash::ToFloat01(CH) > OpParams.ChimneyDensity) continue;

                uint32 CH2 = VoxelHash::Mix(CH  ^ 0x1337u);
                uint32 CH3 = VoxelHash::Mix(CH2 ^ 0xCAFEu);
                uint32 CH4 = VoxelHash::Mix(CH3 ^ 0xD00Du);

                float CX = CR.Center.X + VoxelHash::ToFloatSigned(CH2) * CR.RadiusXY * 0.6f;
                float CY = CR.Center.Y + VoxelHash::ToFloatSigned(VoxelHash::Mix(CH2)) * CR.RadiusXY * 0.6f;

                float ChmRadius = FMath::Lerp(OpParams.ChimneyMinRadius, OpParams.ChimneyMaxRadius,
                                              VoxelHash::ToFloat01(CH3));

                float ChmBottomZ = CR.Center.Z + CR.RadiusZ * 0.5f
                                   - VoxelHash::ToFloat01(CH4) * CR.RadiusZ * 0.2f;

                FCachedChimney Chim;
                Chim.CenterX       = CX;
                Chim.CenterY       = CY;
                Chim.BottomZ       = ChmBottomZ;
                Chim.Radius        = ChmRadius;
                Chim.Height        = OpParams.ChimneyHeight;
                Chim.FlareDist     = ChmRadius * 2.0f;
                Chim.FlareExtra    = ChmRadius * 1.0f;
                Chim.BaseDensity   = Params.BaseDensity;
                Chim.BlendK        = Params.SDFBlendRadius;
                float MaxXYR       = ChmRadius + ChmRadius + Params.SDFBlendRadius + 4.0f;
                Chim.BoundXYRadiusSq = MaxXYR * MaxXYR;
                OutCache.Chimneys.Add(Chim);
            }
        }

        // COLUMNS
        if (OpParams.ColumnDensity > 0.0f)
        {
            const int32 MaxCols = 4;
            for (int32 i = 0; i < MaxCols; i++)
            {
                uint32 H  = VoxelHash::Mix(BR.Hash ^ (0xC01C01u + (uint32)i * 3571u));
                if (VoxelHash::ToFloat01(H) > OpParams.ColumnDensity) continue;

                uint32 H2 = VoxelHash::Mix(H  ^ 0x1A2B3Cu);

                float ColX = CR.Center.X + VoxelHash::ToFloatSigned(H2) * CR.RadiusXY * 0.75f;
                float ColY = CR.Center.Y + VoxelHash::ToFloatSigned(VoxelHash::Mix(H2)) * CR.RadiusXY * 0.75f;

                uint32 H3     = VoxelHash::Mix(H2 ^ 0xBEEFu);
                float ColR    = FMath::Lerp(OpParams.ColumnMinRadius, OpParams.ColumnMaxRadius,
                                            VoxelHash::ToFloat01(H3));

                FCachedColumn Col;
                Col.CenterX       = ColX;
                Col.CenterY       = ColY;
                Col.Radius        = ColR;
                Col.BaseDensity   = Params.BaseDensity;
                float MaxXYR      = ColR + 6.0f;
                Col.BoundXYRadiusSq = MaxXYR * MaxXYR;
                OutCache.Columns.Add(Col);
            }
        }
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
    float RoomShapeVariety,
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

        // --- SHAPE SELECTION ---
        float RoomSDF;
        const uint32 ShapeHash = VoxelHash::Mix(Room.Hash ^ 0xDEADBEEFu);
        const float ShapeRoll = Room.bIsOrigin ? 0.0f : VoxelHash::ToFloat01(ShapeHash);

        // Thresholds: Variety=0 → all ellipsoid. Variety=1 → 50/30/20 split.
        const float BoxThreshold = 1.0f - RoomShapeVariety * 0.5f;
        const float CapsuleThreshold = 1.0f - RoomShapeVariety * 0.2f;

        if (ShapeRoll >= BoxThreshold && ShapeRoll < CapsuleThreshold)
        {
            // ROUNDED BOX: angular chamber with smooth corners
            FVector HalfExtent(
                Room.RadiusXY * 0.8f,
                Room.RadiusXY * 0.8f,
                Room.RadiusZ * 0.8f
            );
            float Rounding = Room.RadiusXY * 0.25f;
            RoomSDF = VoxelSDF::RoundedBox(Pos, Room.Center, HalfExtent, Rounding);
        }
        else if (ShapeRoll >= CapsuleThreshold)
        {
            // ELONGATED CAPSULE: stretched hall/corridor-room
            float DirAngle = VoxelHash::ToFloat01(VoxelHash::Mix(Room.Hash ^ 0xCAFEBABEu)) * 2.0f * PI;
            float StretchDist = Room.RadiusXY * 0.7f;
            FVector Dir(FMath::Cos(DirAngle), FMath::Sin(DirAngle), 0.0f);
            FVector EndA = Room.Center + Dir * StretchDist;
            FVector EndB = Room.Center - Dir * StretchDist;
            float CapsuleR = FMath::Min(Room.RadiusXY * 0.6f, Room.RadiusZ);
            RoomSDF = VoxelSDF::Capsule(Pos, EndA, EndB, CapsuleR);
        }
        else
        {
            // ELLIPSOID (default): smooth oval chamber
            const FVector Radii(Room.RadiusXY, Room.RadiusXY, Room.RadiusZ);
            RoomSDF = VoxelSDF::Ellipsoid(Pos, Room.Center, Radii);
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
    const float Margin = FMath::Max(
        Params.MaxRoomRadius,
        Params.TunnelWarpStrength + Params.TunnelMaxRadius
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
        TempCache, Params.SDFBlendRadius, Params.RoomShapeVariety
    );
}
