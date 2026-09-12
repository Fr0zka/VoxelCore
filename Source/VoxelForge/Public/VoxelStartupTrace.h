// Bounded, opt-in startup trace for the complete streaming path.
// The trace is deliberately independent of the shared Unreal log: callers provide an
// absolute output path and every record is one JSON object per line.
#pragma once

#include "CoreMinimal.h"

namespace VoxelForgeStartupTrace
{
    struct VOXELFORGE_API FTileSample
    {
        int32 Level = -1;
        int32 Verdict = -1; // Mixed=0, AllSolid=1, AllAir=2, -1=not classified (sheet/early path)
        bool bEmpty = true;
        bool bCacheHit = false;
        bool bRegionHit = false;
        int32 Triangles = 0;
        // This is worker-result -> game-thread mesh submission. Collision readiness is a later
        // per-tile RMC completion event recorded separately as collision_ready.
        double RequestToApplySeconds = 0.0;
        double QueueWaitSeconds = 0.0;
        double WorkerQueueSeconds = 0.0;
        double ResultQueueSeconds = 0.0;
        double GenerationSeconds = 0.0;
        double ClassifySeconds = 0.0;
        double MeshSeconds = 0.0;
        double StreamSeconds = 0.0;
        double ApplySeconds = 0.0;
    };

    /** Starts when -voxel.StartupTraceFile=<absolute path> is present. */
    VOXELFORGE_API void BeginFromCommandLine();

    VOXELFORGE_API bool IsActive();

    /** FieldsJson is the contents after the type/timestamp object prefix, without braces. */
    VOXELFORGE_API void RecordEvent(const TCHAR* Type, const FString& FieldsJson = FString());

    VOXELFORGE_API void StageBegin(const TCHAR* Name);
    VOXELFORGE_API void StageEnd(const TCHAR* Name);

    VOXELFORGE_API void RecordTile(const FTileSample& Sample);

    /** Writes the final bounded summary and closes the file. Safe to call more than once. */
    VOXELFORGE_API void Finish(const TCHAR* Reason);

    /** RAII helper for stages which have early-return failure paths. */
    class VOXELFORGE_API FStageScope
    {
    public:
        explicit FStageScope(const TCHAR* InName);
        ~FStageScope();

        FStageScope(const FStageScope&) = delete;
        FStageScope& operator=(const FStageScope&) = delete;

    private:
        FString Name;
        bool bActive = false;
    };
}
