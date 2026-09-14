#include "VoxelStartupTrace.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace VoxelForgeStartupTrace
{
namespace
{
    struct FState
    {
        FArchive* Archive = nullptr;
        FString Path;
        double StartSeconds = 0.0;
        int32 TileRecordCap = 4096;
        int32 TileRecordsSeen = 0;
        int32 TileRecordsWritten = 0;
        int32 TileRecordsSuppressed = 0;
        TArray<FTileSample> Samples;
        TMap<FString, double> StageStarts;
        TMap<FString, double> StageTotals;
        TArray<FString> StageOrder;
        bool bActive = false;
    };

    FCriticalSection GTraceLock;
    TUniquePtr<FState> GState;

    FString JsonEscape(const FString& In)
    {
        FString Out = In;
        Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
        Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
        Out.ReplaceInline(TEXT("\r"), TEXT("\\r"));
        Out.ReplaceInline(TEXT("\n"), TEXT("\\n"));
        return Out;
    }

    FString Number(double Value)
    {
        return FString::SanitizeFloat(Value);
    }

    void WriteLineLocked(FState& State, const FString& Line)
    {
        if (State.Archive == nullptr) return;
        const FString WithNewline = Line + TEXT("\n");
        FTCHARToUTF8 Utf8(*WithNewline);
        State.Archive->Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
        // The trace is intended to remain readable if a harness terminates the editor after
        // the steady-state record. This is bounded I/O (startup only), not the shared UE log.
        State.Archive->Flush();
    }

    void WriteEventLocked(FState& State, const TCHAR* Type, const FString& FieldsJson)
    {
        FString Line = FString::Printf(
            TEXT("{\"type\":\"%s\",\"t_s\":%s"),
            Type,
            *Number(FPlatformTime::Seconds() - State.StartSeconds));
        if (!FieldsJson.IsEmpty())
        {
            Line += TEXT(",");
            Line += FieldsJson;
        }
        Line += TEXT("}");
        WriteLineLocked(State, Line);
    }

    double Percentile(TArray<double> Values, double Fraction)
    {
        if (Values.Num() == 0) return 0.0;
        Values.Sort();
        const int32 Index = FMath::Clamp(
            FMath::CeilToInt(Fraction * static_cast<double>(Values.Num() - 1)),
            0,
            Values.Num() - 1);
        return Values[Index];
    }

    double Maximum(const TArray<double>& Values)
    {
        double Result = 0.0;
        for (const double Value : Values)
        {
            Result = FMath::Max(Result, Value);
        }
        return Result;
    }

    FString BuildTileGroupJson(const TArray<FTileSample>& Samples, int32 Level)
    {
        TArray<double> Request, Queue, Generation, Classify, Mesh, Apply;
        int32 Count = 0;
        int32 Empty = 0;
        int32 AllAir = 0;
        int32 AllSolid = 0;
        int32 Mixed = 0;
        int64 Triangles = 0;

        for (const FTileSample& Sample : Samples)
        {
            if (Level >= 0 && Sample.Level != Level) continue;
            ++Count;
            if (Sample.bEmpty) ++Empty;
            if (Sample.Verdict == 0) ++Mixed;
            else if (Sample.Verdict == 1) ++AllSolid;
            else if (Sample.Verdict == 2) ++AllAir;
            Triangles += Sample.Triangles;
            Request.Add(Sample.RequestToApplySeconds);
            Queue.Add(Sample.QueueWaitSeconds);
            Generation.Add(Sample.GenerationSeconds);
            Classify.Add(Sample.ClassifySeconds);
            Mesh.Add(Sample.MeshSeconds);
            Apply.Add(Sample.ApplySeconds);
        }

        return FString::Printf(
            TEXT("{\"count\":%d,\"empty\":%d,\"all_air\":%d,\"all_solid\":%d,\"mixed\":%d,\"triangles\":%lld,"
                 "\"request_apply_s\":{\"p50\":%s,\"p95\":%s,\"max\":%s},"
                 "\"queue_s\":{\"p50\":%s,\"p95\":%s,\"max\":%s},"
                 "\"generation_s\":{\"p50\":%s,\"p95\":%s,\"max\":%s},"
                 "\"classify_s\":{\"p50\":%s,\"p95\":%s,\"max\":%s},"
                 "\"mesh_s\":{\"p50\":%s,\"p95\":%s,\"max\":%s},"
                 "\"apply_s\":{\"p50\":%s,\"p95\":%s,\"max\":%s}}"),
            Count, Empty, AllAir, AllSolid, Mixed, static_cast<long long>(Triangles),
            *Number(Percentile(Request, 0.50)), *Number(Percentile(Request, 0.95)), *Number(Maximum(Request)),
            *Number(Percentile(Queue, 0.50)), *Number(Percentile(Queue, 0.95)), *Number(Maximum(Queue)),
            *Number(Percentile(Generation, 0.50)), *Number(Percentile(Generation, 0.95)), *Number(Maximum(Generation)),
            *Number(Percentile(Classify, 0.50)), *Number(Percentile(Classify, 0.95)), *Number(Maximum(Classify)),
            *Number(Percentile(Mesh, 0.50)), *Number(Percentile(Mesh, 0.95)), *Number(Maximum(Mesh)),
            *Number(Percentile(Apply, 0.50)), *Number(Percentile(Apply, 0.95)), *Number(Maximum(Apply)));
    }

    FString BuildSummaryFieldsLocked(const FState& State, const TCHAR* Reason)
    {
        FString Stages = TEXT("{");
        for (int32 Index = 0; Index < State.StageOrder.Num(); ++Index)
        {
            if (Index > 0) Stages += TEXT(",");
            const FString& Name = State.StageOrder[Index];
            const double* Total = State.StageTotals.Find(Name);
            Stages += FString::Printf(
                TEXT("\"%s\":%s"), *JsonEscape(Name), *Number(Total ? *Total : 0.0));
        }
        Stages += TEXT("}");

        TArray<int32> Levels;
        for (const FTileSample& Sample : State.Samples)
        {
            if (Sample.Level >= 0 && !Levels.Contains(Sample.Level)) Levels.Add(Sample.Level);
        }
        Levels.Sort();

        FString Groups = FString::Printf(
            TEXT("\"all\":%s"), *BuildTileGroupJson(State.Samples, -1));
        for (const int32 Level : Levels)
        {
            Groups += FString::Printf(
                TEXT(",\"lod%d\":%s"), Level, *BuildTileGroupJson(State.Samples, Level));
        }

        return FString::Printf(
            TEXT("\"reason\":\"%s\",\"wall_s\":%s,\"tile_records_seen\":%d,"
                 "\"tile_records_written\":%d,\"tile_records_suppressed\":%d,"
                 "\"tile_record_cap\":%d,\"stage_seconds\":%s,\"tile_percentiles\":{%s}"),
            *JsonEscape(Reason ? FString(Reason) : FString(TEXT("unknown"))),
            *Number(FPlatformTime::Seconds() - State.StartSeconds),
            State.TileRecordsSeen, State.TileRecordsWritten, State.TileRecordsSuppressed,
            State.TileRecordCap, *Stages, *Groups);
    }
}

void BeginFromCommandLine()
{
    FScopeLock Lock(&GTraceLock);
    if (GState.IsValid()) return;

    FString Path;
    if (!FParse::Value(FCommandLine::Get(), TEXT("voxel.StartupTraceFile="), Path)) return;
    Path.TrimQuotesInline();
    if (Path.IsEmpty()) return;

    TUniquePtr<FState> NewState = MakeUnique<FState>();
    NewState->Path = Path;
    FParse::Value(FCommandLine::Get(), TEXT("voxel.StartupTraceMaxTiles="), NewState->TileRecordCap);
    NewState->TileRecordCap = FMath::Clamp(NewState->TileRecordCap, 64, 16384);
    NewState->Samples.Reserve(NewState->TileRecordCap);
    NewState->Archive = IFileManager::Get().CreateFileWriter(*Path);
    if (NewState->Archive == nullptr)
    {
        return;
    }

    NewState->StartSeconds = FPlatformTime::Seconds();
    NewState->bActive = true;
    GState = MoveTemp(NewState);
    WriteEventLocked(*GState, TEXT("header"), FString::Printf(
        TEXT("\"schema\":1,\"path\":\"%s\",\"tile_record_cap\":%d"),
        *JsonEscape(GState->Path), GState->TileRecordCap));
}

bool IsActive()
{
    FScopeLock Lock(&GTraceLock);
    return GState.IsValid() && GState->bActive;
}

void RecordEvent(const TCHAR* Type, const FString& FieldsJson)
{
    FScopeLock Lock(&GTraceLock);
    if (!GState.IsValid() || !GState->bActive) return;
    WriteEventLocked(*GState, Type, FieldsJson);
}

void StageBegin(const TCHAR* Name)
{
    if (Name == nullptr) return;
    FScopeLock Lock(&GTraceLock);
    if (!GState.IsValid() || !GState->bActive) return;
    const FString StageName(Name);
    if (!GState->StageStarts.Contains(StageName))
    {
        GState->StageOrder.Add(StageName);
    }
    GState->StageStarts.Add(StageName, FPlatformTime::Seconds());
    WriteEventLocked(*GState, TEXT("stage_begin"), FString::Printf(
        TEXT("\"name\":\"%s\""), *JsonEscape(StageName)));
}

void StageEnd(const TCHAR* Name)
{
    if (Name == nullptr) return;
    FScopeLock Lock(&GTraceLock);
    if (!GState.IsValid() || !GState->bActive) return;
    const FString StageName(Name);
    const double Now = FPlatformTime::Seconds();
    const double* Start = GState->StageStarts.Find(StageName);
    const double Duration = Start ? FMath::Max(0.0, Now - *Start) : 0.0;
    GState->StageTotals.FindOrAdd(StageName) += Duration;
    GState->StageStarts.Remove(StageName);
    WriteEventLocked(*GState, TEXT("stage_end"), FString::Printf(
        TEXT("\"name\":\"%s\",\"duration_s\":%s"),
        *JsonEscape(StageName), *Number(Duration)));
}

void RecordTile(const FTileSample& Sample)
{
    FScopeLock Lock(&GTraceLock);
    if (!GState.IsValid() || !GState->bActive) return;
    ++GState->TileRecordsSeen;
    if (GState->TileRecordsWritten >= GState->TileRecordCap)
    {
        ++GState->TileRecordsSuppressed;
        return;
    }

    GState->Samples.Add(Sample);
    ++GState->TileRecordsWritten;
    WriteEventLocked(*GState, TEXT("tile"), FString::Printf(
        TEXT("\"tile\":[%d,%d,%d],\"level\":%d,\"verdict\":%d,\"proof\":%d,\"empty\":%d,\"triangles\":%d,"
             "\"request_to_apply_s\":%s,\"queue_wait_s\":%s,\"worker_queue_s\":%s,\"result_queue_s\":%s,"
             "\"generation_s\":%s,\"classify_s\":%s,\"mesh_s\":%s,\"stream_s\":%s,\"apply_s\":%s"),
        Sample.TileX, Sample.TileY, Sample.TileZ,
        Sample.Level, Sample.Verdict, Sample.bSealedSolidProof ? 1 : 0,
        Sample.bEmpty ? 1 : 0, Sample.Triangles,
        *Number(Sample.RequestToApplySeconds), *Number(Sample.QueueWaitSeconds),
        *Number(Sample.WorkerQueueSeconds), *Number(Sample.ResultQueueSeconds),
        *Number(Sample.GenerationSeconds), *Number(Sample.ClassifySeconds),
        *Number(Sample.MeshSeconds), *Number(Sample.StreamSeconds), *Number(Sample.ApplySeconds)));
}

void Finish(const TCHAR* Reason)
{
    FScopeLock Lock(&GTraceLock);
    if (!GState.IsValid() || !GState->bActive) return;

    // Close any stage left open by an abnormal early exit so the summary remains useful.
    const double Now = FPlatformTime::Seconds();
    for (const TPair<FString, double>& Pair : GState->StageStarts)
    {
        GState->StageTotals.FindOrAdd(Pair.Key) += FMath::Max(0.0, Now - Pair.Value);
    }
    GState->StageStarts.Reset();
    WriteEventLocked(*GState, TEXT("summary"), BuildSummaryFieldsLocked(*GState, Reason));
    GState->Archive->Flush();
    delete GState->Archive;
    GState->Archive = nullptr;
    GState->bActive = false;

    // The harness polls this tiny sentinel instead of opening the live JSONL. Unreal's file
    // writer does not allow a concurrent reader on Windows; create it only after the trace is
    // closed so the summary is safe to consume.
    const FString DonePath = GState->Path + TEXT(".done");
    if (FArchive* DoneArchive = IFileManager::Get().CreateFileWriter(*DonePath))
    {
        const ANSICHAR DoneText[] = "summary_complete\n";
        DoneArchive->Serialize(const_cast<ANSICHAR*>(DoneText), UE_ARRAY_COUNT(DoneText) - 1);
        DoneArchive->Flush();
        delete DoneArchive;
    }
}

FStageScope::FStageScope(const TCHAR* InName)
    : Name(InName ? InName : TEXT("unnamed"))
    , bActive(IsActive())
{
    if (bActive) StageBegin(*Name);
}

FStageScope::~FStageScope()
{
    if (bActive) StageEnd(*Name);
}
}
