// VoxelStackSampler.cpp
// Fixed-storage raw stack sampling for VoxelForge generation workers.

#include "VoxelStackSampler.h"

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformStackWalk.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"
#include "Containers/StringConv.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/Archive.h"
#include "Templates/UniquePtr.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsPlatformStackWalk.h"
#endif

#include <atomic>
#include <utility>

namespace
{
constexpr uint64 RegistrationReservedBit = 1ull << 63;
constexpr uint32 MaxRegisteredThreads = 128;

std::atomic<bool> GVoxelStackSamplerEnabled(false);

// The registry is deliberately fixed-size and lock-free from the worker's point of view.  A
// reserved state prevents two registering workers from racing on ThreadId before a slot becomes
// visible to the sampler.
struct FRegisteredThreadSlot
{
    std::atomic<uint64> State{0};
    std::atomic<uint64> ThreadId{0};
    std::atomic<int32> LODLevel{INDEX_NONE};
};

thread_local int32 GTlsRegisteredSlot = INDEX_NONE;
thread_local uint64 GTlsRegistrationGeneration = 0;
thread_local int32 GTlsRegistrationDepth = 0;
thread_local int32 GTlsRegisteredLODLevel = INDEX_NONE;
thread_local void* GTlsRegisteredImpl = nullptr;
thread_local uint64 GTlsSamplerSessionGeneration = 0;

std::atomic<uint64> GNextSamplerSessionGeneration(1);

void ResetRegistrationTls()
{
    GTlsRegisteredSlot = INDEX_NONE;
    GTlsRegistrationGeneration = 0;
    GTlsRegistrationDepth = 0;
    GTlsRegisteredLODLevel = INDEX_NONE;
    GTlsRegisteredImpl = nullptr;
    GTlsSamplerSessionGeneration = 0;
}

FString SanitizeLabel(const FString& InLabel)
{
    FString Result;
    for (const TCHAR Character : InLabel)
    {
        const bool bAlphaNumeric =
            (Character >= TEXT('a') && Character <= TEXT('z'))
            || (Character >= TEXT('A') && Character <= TEXT('Z'))
            || (Character >= TEXT('0') && Character <= TEXT('9'));
        Result.AppendChar(bAlphaNumeric || Character == TEXT('_') || Character == TEXT('-')
            ? Character : TEXT('_'));
    }
    return Result.IsEmpty() ? TEXT("run") : Result;
}

void WriteUtf8(FArchive& Archive, const FString& Text)
{
    FTCHARToUTF8 Utf8(*Text);
    if (Utf8.Length() > 0)
    {
        Archive.Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
    }
}

FString NormalizedForClassification(const FString& InValue)
{
    FString Result = InValue;
    Result.ToLowerInline();
    Result.ReplaceInline(TEXT("\\"), TEXT("/"));
    return Result;
}

struct FResolvedSymbol
{
    FString Module;
    FString Function;
    FString File;
    int32 Line = 0;
    bool bInline = false;
};

struct FResolvedProgramCounter
{
    bool bAttempted = false;
    TArray<FResolvedSymbol> Symbols;
};

struct FProfileRow
{
    FString Key;
    FString Function;
    FString Module;
    FString File;
    FString Label;
    int32 Line = 0;
    uint64 Samples = 0;
};

struct FProfileAggregateSet
{
    TMap<FString, FProfileRow> ExclusiveFunctions;
    TMap<FString, FProfileRow> InclusiveFunctions;
    TMap<FString, FProfileRow> ExclusiveLines;
    uint64 SamplesWithStack = 0;
};

const FResolvedSymbol* FirstFunctionSymbol(const FResolvedProgramCounter& Entry)
{
    for (const FResolvedSymbol& Symbol : Entry.Symbols)
    {
        if (!Symbol.Function.IsEmpty())
        {
            return &Symbol;
        }
    }
    return nullptr;
}

const FResolvedSymbol* FirstSourceSymbol(const FResolvedProgramCounter& Entry)
{
    for (const FResolvedSymbol& Symbol : Entry.Symbols)
    {
        if (!Symbol.File.IsEmpty() && Symbol.Line > 0)
        {
            return &Symbol;
        }
    }
    return nullptr;
}

FString SymbolLabel(const FResolvedSymbol& Symbol)
{
    if (Symbol.Function.IsEmpty())
    {
        return TEXT("unknown");
    }

    const FString FunctionLower = NormalizedForClassification(Symbol.Function);
    if (FunctionLower.Contains(TEXT("std::")))
    {
        return TEXT("std");
    }

    const FString ModuleLower = NormalizedForClassification(Symbol.Module);
    const FString FileLower = NormalizedForClassification(Symbol.File);
    const bool bFileIsEngine = FileLower.Contains(TEXT("/engine/"));
    if (bFileIsEngine)
    {
        return TEXT("engine");
    }

    const bool bVoxelForge = FunctionLower.Contains(TEXT("voxelforge"))
        || FileLower.Contains(TEXT("/voxelforge/"))
        || ModuleLower.Contains(TEXT("voxelforge"));
    if (bVoxelForge)
    {
        return TEXT("plugin");
    }

    const bool bModuleIsEngine = ModuleLower.StartsWith(TEXT("unrealeditor-"))
        || ModuleLower.StartsWith(TEXT("ue5-"));
    return bModuleIsEngine ? TEXT("engine") : TEXT("other");
}

bool IsWaitLike(const FResolvedSymbol& Symbol)
{
    const FString FunctionLower = NormalizedForClassification(Symbol.Function);
    return FunctionLower.Contains(TEXT("wait"))
        || FunctionLower.Contains(TEXT("sleep"))
        || FunctionLower.Contains(TEXT("semaphore"))
        || FunctionLower.Contains(TEXT("fevent"));
}

FString FunctionKey(const FResolvedSymbol& Symbol)
{
    return SymbolLabel(Symbol) + TEXT("\x1f") + Symbol.Function;
}

FString SourceKey(const FResolvedSymbol* Symbol)
{
    if (Symbol == nullptr || Symbol->File.IsEmpty() || Symbol->Line <= 0)
    {
        return TEXT("unknown\x1f<unknown>");
    }
    return SymbolLabel(*Symbol) + TEXT("\x1f")
        + FString::Printf(TEXT("%s:%d"), *Symbol->File, Symbol->Line);
}

void AddFunctionRow(
    TMap<FString, FProfileRow>& Rows,
    const FResolvedSymbol* Symbol,
    uint64 SampleCount)
{
    if (Symbol == nullptr || Symbol->Function.IsEmpty())
    {
        return;
    }
    const FString Key = FunctionKey(*Symbol);
    FProfileRow& Row = Rows.FindOrAdd(Key);
    if (Row.Key.IsEmpty())
    {
        Row.Key = Key;
        Row.Function = Symbol->Function;
        Row.Module = Symbol->Module;
        Row.File = Symbol->File;
        Row.Line = Symbol->Line;
        Row.Label = SymbolLabel(*Symbol);
    }
    Row.Samples += SampleCount;
}

void AddSourceRow(
    TMap<FString, FProfileRow>& Rows,
    const FResolvedSymbol* FunctionSymbol,
    const FResolvedSymbol* SourceSymbol)
{
    const FString Key = SourceKey(SourceSymbol);
    FProfileRow& Row = Rows.FindOrAdd(Key);
    if (Row.Key.IsEmpty())
    {
        Row.Key = Key;
        Row.Label = SourceSymbol != nullptr ? SymbolLabel(*SourceSymbol) : TEXT("unknown");
        Row.File = SourceSymbol != nullptr ? SourceSymbol->File : TEXT("<unknown>");
        Row.Line = SourceSymbol != nullptr ? SourceSymbol->Line : 0;
        Row.Function = FunctionSymbol != nullptr ? FunctionSymbol->Function : TEXT("<unknown>");
        Row.Module = FunctionSymbol != nullptr ? FunctionSymbol->Module : TEXT("<unknown>");
    }
    ++Row.Samples;
}

TArray<FProfileRow> SortedRows(const TMap<FString, FProfileRow>& InRows)
{
    TArray<FProfileRow> Rows;
    Rows.Reserve(InRows.Num());
    for (const TPair<FString, FProfileRow>& Pair : InRows)
    {
        Rows.Add(Pair.Value);
    }
    Rows.Sort([](const FProfileRow& A, const FProfileRow& B)
    {
        return A.Samples != B.Samples ? A.Samples > B.Samples : A.Key < B.Key;
    });
    return Rows;
}

void AppendRows(
    FString& Out,
    const TCHAR* Title,
    const TMap<FString, FProfileRow>& InRows,
    uint64 Denominator,
    int32 MaxRows)
{
    Out += TEXT("\n");
    Out += Title;
    Out += TEXT("\nrank\tsamples\tpercent\tlabel\tfunction\tmodule\tfile\tline\n");
    const TArray<FProfileRow> Rows = SortedRows(InRows);
    const int32 Count = FMath::Min(MaxRows, Rows.Num());
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const FProfileRow& Row = Rows[Index];
        const double Percent = Denominator > 0
            ? static_cast<double>(Row.Samples) * 100.0 / static_cast<double>(Denominator)
            : 0.0;
        Out.Appendf(
            TEXT("%d\t%llu\t%.4f\t%s\t%s\t%s\t%s\t%d\n"),
            Index + 1,
            static_cast<unsigned long long>(Row.Samples),
            Percent,
            *Row.Label,
            *Row.Function,
            *Row.Module,
            *Row.File,
            Row.Line);
    }
    if (Rows.Num() == 0)
    {
        Out += TEXT("(no rows)\n");
    }
}
}

struct FVoxelStackSampler::FImpl final : public FRunnable
{
    struct FStoredSample
    {
        uint64 ElapsedUs = 0;
        uint64 ThreadId = 0;
        int32 LODLevel = INDEX_NONE;
        uint32 Depth = 0;
        uint64 ProgramCounters[MaxStackDepth]{};
    };

    FVoxelStackSampler& Owner;
    uint32 IntervalUs = DefaultIntervalUs;
    FString RunLabel;
    FString OutputDirectory;
    FString RawSamplesPath;
    FString SummaryPath;
    double StartSeconds = 0.0;
    std::atomic<bool> bStop{false};
    uint64 SamplerSessionGeneration = 0;
    std::atomic<uint64> NextRegistrationGeneration{1};
    FRegisteredThreadSlot Slots[MaxRegisteredThreads];
    TUniquePtr<FStoredSample[]> Samples;
    uint32 StoredSampleCount = 0;

    uint64 SamplerTicks = 0;
    uint64 CaptureAttempts = 0;
    uint64 RegisteredThreadSamples = 0;
    uint64 RegistrationRaceDrops = 0;
    uint64 CaptureFailures = 0;
    uint64 DroppedSamples = 0;
    uint64 TotalFrames = 0;
    uint64 ReservoirState = 0x9E3779B97F4A7C15ull;
    uint64 MaxActiveRegisteredThreads = 0;
    bool bStackWalkingInitialized = false;
    FRunnableThread* Thread = nullptr;

    FImpl(
        FVoxelStackSampler& InOwner,
        uint32 InIntervalUs,
        const FString& InOutputDirectory,
        const FString& InRunLabel)
        : Owner(InOwner)
        , IntervalUs(FMath::Clamp(InIntervalUs, MinIntervalUs, MaxIntervalUs))
        , RunLabel(InRunLabel)
        , OutputDirectory(InOutputDirectory)
    {
        SamplerSessionGeneration = GNextSamplerSessionGeneration.fetch_add(
            1, std::memory_order_relaxed);
        const FString SafeLabel = SanitizeLabel(RunLabel);
        const uint32 ProcessId = FPlatformProcess::GetCurrentProcessId();
        RawSamplesPath = FPaths::Combine(
            OutputDirectory,
            FString::Printf(TEXT("VoxelStackSamples_%s_%u.csv"), *SafeLabel, ProcessId));
        SummaryPath = FPaths::Combine(
            OutputDirectory,
            FString::Printf(TEXT("VoxelStackSummary_%s_%u.txt"), *SafeLabel, ProcessId));
        Samples = TUniquePtr<FStoredSample[]>(new FStoredSample[MaxStoredSamples]);
    }

    virtual uint32 Run() override
    {
#if PLATFORM_WINDOWS
        StartSeconds = FPlatformTime::Seconds();
        double NextSampleSeconds = StartSeconds;
        while (!bStop.load(std::memory_order_relaxed))
        {
            const double Now = FPlatformTime::Seconds();
            if (Now < NextSampleSeconds)
            {
                const double SleepSeconds = NextSampleSeconds - Now;
                FPlatformProcess::SleepNoStats(static_cast<float>(FMath::Min(SleepSeconds, 0.001)));
                continue;
            }

            CaptureRegisteredThreads(Now);
            ++SamplerTicks;
            NextSampleSeconds += static_cast<double>(IntervalUs) * 1.0e-6;
            if (NextSampleSeconds < Now)
            {
                NextSampleSeconds = Now + static_cast<double>(IntervalUs) * 1.0e-6;
            }
        }
#else
        // The requested implementation is Windows-specific.  Keep the runtime module buildable
        // on other targets, but never pretend that a no-op is a profile.
        while (!bStop.load(std::memory_order_relaxed))
        {
            FPlatformProcess::SleepNoStats(0.01f);
        }
#endif
        return 0;
    }

    virtual void Stop() override
    {
        bStop.store(true, std::memory_order_release);
    }

    bool StartThread()
    {
#if PLATFORM_WINDOWS
        // CaptureThreadStackBackTrace uses the same-process context path and deliberately does
        // not require DbgHelp. Initializing DbgHelp here made the measured process depend on
        // global symbol-engine state before shutdown; symbolization is an offline concern.
        StartSeconds = FPlatformTime::Seconds();
#endif
        Thread = FRunnableThread::Create(this, TEXT("VoxelStackSampler"), 0, TPri_Lowest);
        return Thread != nullptr;
    }

    void StopThread()
    {
        if (Thread == nullptr)
        {
            return;
        }
        bStop.store(true, std::memory_order_release);
        Thread->WaitForCompletion();
        delete Thread;
        Thread = nullptr;
    }

    bool RegisterThread(int32 LODLevel)
    {
        const uint32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();
        if (GTlsRegistrationDepth > 0)
        {
            if (GTlsRegisteredImpl == this
                && GTlsSamplerSessionGeneration == SamplerSessionGeneration)
            {
                ++GTlsRegistrationDepth;
                return true;
            }

            // A worker can outlive a session's owner-side shutdown. Never let stale TLS from
            // that session masquerade as a nested registration in a later session.
            ResetRegistrationTls();
        }

        const uint64 Generation = NextRegistrationGeneration.fetch_add(
            1, std::memory_order_relaxed);
        const uint64 ReservedState = Generation | RegistrationReservedBit;
        for (uint32 Index = 0; Index < MaxRegisteredThreads; ++Index)
        {
            uint64 Expected = 0;
            if (!Slots[Index].State.compare_exchange_strong(
                    Expected, ReservedState,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                continue;
            }

            Slots[Index].ThreadId.store(CurrentThreadId, std::memory_order_relaxed);
            Slots[Index].LODLevel.store(LODLevel, std::memory_order_relaxed);
            Slots[Index].State.store(Generation, std::memory_order_release);
            GTlsRegisteredSlot = static_cast<int32>(Index);
            GTlsRegistrationGeneration = Generation;
            GTlsRegistrationDepth = 1;
            GTlsRegisteredLODLevel = LODLevel;
            GTlsRegisteredImpl = this;
            GTlsSamplerSessionGeneration = SamplerSessionGeneration;
            return true;
        }
        return false;
    }

    void DeregisterThread()
    {
        if (GTlsRegistrationDepth <= 0)
        {
            return;
        }
        --GTlsRegistrationDepth;
        if (GTlsRegistrationDepth > 0)
        {
            return;
        }

        if (GTlsRegisteredSlot >= 0 && GTlsRegisteredSlot < static_cast<int32>(MaxRegisteredThreads))
        {
            FRegisteredThreadSlot& Slot = Slots[GTlsRegisteredSlot];
            const uint64 State = Slot.State.load(std::memory_order_acquire);
            if (State == GTlsRegistrationGeneration)
            {
                Slot.State.store(0, std::memory_order_release);
                Slot.ThreadId.store(0, std::memory_order_relaxed);
                Slot.LODLevel.store(INDEX_NONE, std::memory_order_relaxed);
            }
        }
        ResetRegistrationTls();
    }

    void CaptureRegisteredThreads(double NowSeconds)
    {
#if PLATFORM_WINDOWS
        uint64 BackTrace[MaxStackDepth]{};
        uint64 ActiveCount = 0;
        for (uint32 Index = 0; Index < MaxRegisteredThreads; ++Index)
        {
            FRegisteredThreadSlot& Slot = Slots[Index];
            const uint64 Generation = Slot.State.load(std::memory_order_acquire);
            if (Generation == 0 || (Generation & RegistrationReservedBit) != 0)
            {
                continue;
            }
            const uint64 ThreadId = Slot.ThreadId.load(std::memory_order_acquire);
            if (ThreadId == 0)
            {
                continue;
            }
            const int32 LODLevel = Slot.LODLevel.load(std::memory_order_acquire);
            ++ActiveCount;
            ++CaptureAttempts;
            const uint32 CapturedDepth = FPlatformStackWalk::CaptureThreadStackBackTrace(
                ThreadId, BackTrace, MaxStackDepth);
            const uint64 EndGeneration = Slot.State.load(std::memory_order_acquire);
            const uint64 EndThreadId = Slot.ThreadId.load(std::memory_order_acquire);
            const int32 EndLODLevel = Slot.LODLevel.load(std::memory_order_acquire);
            if (EndGeneration != Generation || EndThreadId != ThreadId || EndLODLevel != LODLevel)
            {
                ++RegistrationRaceDrops;
                continue;
            }

            ++RegisteredThreadSamples;
            const uint32 Depth = FMath::Min(CapturedDepth, MaxStackDepth);
            if (Depth == 0)
            {
                ++CaptureFailures;
            }
            StoreSample(
                NowSeconds,
                ThreadId,
                LODLevel,
                Depth,
                BackTrace);
        }
        MaxActiveRegisteredThreads = FMath::Max(MaxActiveRegisteredThreads, ActiveCount);
#else
        (void)NowSeconds;
#endif
    }

    uint64 NextReservoirValue()
    {
        ReservoirState ^= ReservoirState << 7;
        ReservoirState ^= ReservoirState >> 9;
        ReservoirState ^= ReservoirState << 8;
        return ReservoirState;
    }

    void StoreSample(
        double NowSeconds,
        uint64 ThreadId,
        int32 LODLevel,
        uint32 Depth,
        const uint64* BackTrace)
    {
        FStoredSample* Sample = nullptr;
        if (StoredSampleCount < MaxStoredSamples)
        {
            Sample = &Samples[StoredSampleCount++];
        }
        else
        {
            ++DroppedSamples;
            // RegisteredThreadSamples is the one-based ordinal of this stable capture. Select a
            // uniform-ish prior slot; the raw store therefore represents the full run after the
            // bound is reached without allocating or synchronizing with workers.
            const uint64 Candidate = NextReservoirValue() % RegisteredThreadSamples;
            if (Candidate >= MaxStoredSamples)
            {
                return;
            }
            Sample = &Samples[Candidate];
            TotalFrames -= Sample->Depth;
        }

        Sample->ElapsedUs = NowSeconds >= StartSeconds
            ? static_cast<uint64>((NowSeconds - StartSeconds) * 1.0e6)
            : 0;
        Sample->ThreadId = ThreadId;
        Sample->LODLevel = LODLevel;
        Sample->Depth = Depth;
        TotalFrames += Depth;
        if (Depth > 0)
        {
            FMemory::Memcpy(
                Sample->ProgramCounters,
                BackTrace,
                static_cast<SIZE_T>(Depth) * sizeof(uint64));
        }
    }

    FSummary Finalize()
    {
        FSummary Result;
        Result.bSupported = PLATFORM_WINDOWS != 0;
        Result.bStarted = true;
        Result.IntervalUs = IntervalUs;
        Result.MaxDepth = MaxStackDepth;
        Result.MaxStoredSampleCount = MaxStoredSamples;
        Result.bStackWalkingInitialized = bStackWalkingInitialized;
        Result.SamplerTicks = SamplerTicks;
        Result.CaptureAttempts = CaptureAttempts;
        Result.RegisteredThreadSamples = RegisteredThreadSamples;
        Result.RegistrationRaceDrops = RegistrationRaceDrops;
        Result.CaptureFailures = CaptureFailures;
        Result.RetainedSamples = StoredSampleCount;
        Result.DroppedSamples = DroppedSamples;
        Result.TotalFrames = TotalFrames;
        Result.MaxActiveRegisteredThreads = MaxActiveRegisteredThreads;
        Result.RunLabel = RunLabel;
        Result.RawSamplesPath = RawSamplesPath;
        Result.SummaryPath = SummaryPath;
        Result.RunSeconds = FPlatformTime::Seconds() - StartSeconds;

        WriteRawSamples(Result);
        // Do not call the DbgHelp-backed symbolizer from the measured process. DbgHelp is a
        // process-global state machine and can fault during shutdown even after the sampler
        // thread has joined. The raw PCs remain available for a separate offline pass.
        WriteRawOnlySummary(Result);
        return Result;
    }

    void WriteRawSamples(FSummary& Result)
    {
        FArchive* Archive = IFileManager::Get().CreateFileWriter(*RawSamplesPath);
        if (Archive == nullptr)
        {
            return;
        }

        FString Header = TEXT("sample_index,elapsed_us,thread_id,lod,depth");
        for (uint32 Index = 0; Index < MaxStackDepth; ++Index)
        {
            Header += FString::Printf(TEXT(",pc%u"), Index);
        }
        Header += TEXT("\n");
        WriteUtf8(*Archive, Header);

        for (uint32 SampleIndex = 0; SampleIndex < StoredSampleCount; ++SampleIndex)
        {
            const FStoredSample& Sample = Samples[SampleIndex];
            FString Line = FString::Printf(
                TEXT("%u,%llu,%llu,%d,%u"),
                SampleIndex,
                static_cast<unsigned long long>(Sample.ElapsedUs),
                static_cast<unsigned long long>(Sample.ThreadId),
                Sample.LODLevel,
                Sample.Depth);
            for (uint32 FrameIndex = 0; FrameIndex < Sample.Depth; ++FrameIndex)
            {
                Line += FString::Printf(
                    TEXT(",0x%016llX"),
                    static_cast<unsigned long long>(Sample.ProgramCounters[FrameIndex]));
            }
            Line += TEXT("\n");
            WriteUtf8(*Archive, Line);
        }
        delete Archive;
        Result.bOutputWritten = true;
    }

    void WriteRawOnlySummary(FSummary& Result)
    {
        uint64 SamplesWithStack = 0;
        for (uint32 SampleIndex = 0; SampleIndex < StoredSampleCount; ++SampleIndex)
        {
            if (Samples[SampleIndex].Depth > 0)
            {
                ++SamplesWithStack;
            }
        }

        FString Summary;
        Summary += TEXT("VoxelForge raw stack sampler\n");
        Summary += TEXT("============================\n");
        Summary.Appendf(TEXT("run_label: %s\n"), *RunLabel);
        Summary.Appendf(TEXT("supported: %s\n"), Result.bSupported ? TEXT("yes") : TEXT("no"));
        Summary += TEXT("raw_only: yes\n");
        Summary += TEXT("symbolization: deferred\n");
        Summary += TEXT("dbghelp_in_measured_process: no\n");
        Summary.Appendf(TEXT("interval_us: %u\n"), IntervalUs);
        Summary.Appendf(TEXT("max_stack_depth: %u\n"), MaxStackDepth);
        Summary.Appendf(TEXT("max_stored_samples: %u\n"), MaxStoredSamples);
        Summary.Appendf(TEXT("sampler_ticks: %llu\n"),
            static_cast<unsigned long long>(SamplerTicks));
        Summary.Appendf(TEXT("capture_attempts: %llu\n"),
            static_cast<unsigned long long>(CaptureAttempts));
        Summary.Appendf(TEXT("registered_thread_samples: %llu\n"),
            static_cast<unsigned long long>(RegisteredThreadSamples));
        Summary.Appendf(TEXT("registration_race_drops: %llu\n"),
            static_cast<unsigned long long>(RegistrationRaceDrops));
        Summary.Appendf(TEXT("capture_failures: %llu\n"),
            static_cast<unsigned long long>(CaptureFailures));
        Summary.Appendf(TEXT("retained_samples: %llu\n"),
            static_cast<unsigned long long>(StoredSampleCount));
        Summary.Appendf(TEXT("dropped_samples_after_bound: %llu\n"),
            static_cast<unsigned long long>(DroppedSamples));
        Summary.Appendf(TEXT("samples_with_stack: %llu\n"),
            static_cast<unsigned long long>(SamplesWithStack));
        Summary.Appendf(TEXT("total_frames: %llu\n"),
            static_cast<unsigned long long>(TotalFrames));
        Summary.Appendf(TEXT("max_active_registered_threads: %llu\n"),
            static_cast<unsigned long long>(MaxActiveRegisteredThreads));
        Summary.Appendf(TEXT("run_seconds: %.6f\n"), Result.RunSeconds);
        Summary.Appendf(TEXT("raw_samples_path: %s\n"), *RawSamplesPath);
        Summary += TEXT("\nThe target process records bounded raw PCs only; resolve symbols after the process exits.\n");

        if (FFileHelper::SaveStringToFile(
                Summary,
                *SummaryPath,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            Result.bOutputWritten = true;
        }
    }

    void ResolveProgramCounter(
        uint64 ProgramCounter,
        TMap<uint64, FResolvedProgramCounter>& SymbolCache,
        FSummary& Result)
    {
        FResolvedProgramCounter& Entry = SymbolCache.FindOrAdd(ProgramCounter);
        if (Entry.bAttempted)
        {
            return;
        }
        Entry.bAttempted = true;

#if PLATFORM_WINDOWS
        FPlatformStackWalk::EnumerateSymbolInfosForProgramCounter(
            ProgramCounter,
            true,
            [&Entry, &Result](FProgramCounterSymbolInfo& Info)
            {
                FResolvedSymbol Symbol;
                Symbol.Module = FString(ANSI_TO_TCHAR(Info.ModuleName));
                Symbol.Function = FString(ANSI_TO_TCHAR(Info.FunctionName));
                Symbol.File = FString(ANSI_TO_TCHAR(Info.Filename));
                Symbol.Line = Info.LineNumber;
                Symbol.bInline = Symbol.Function.StartsWith(TEXT("[Inline Frame]"));
                if (Symbol.bInline)
                {
                    ++Result.InlineSymbols;
                }
                Entry.Symbols.Add(MoveTemp(Symbol));
            });
        // DbgHelp can return source-only inline records for a PC. Ask the ordinary resolver as
        // well when none of the enumerated records has a function name, so an unresolved frame
        // does not hide an available outer function symbol.
        if (Entry.Symbols.Num() == 0 || FirstFunctionSymbol(Entry) == nullptr)
        {
            FProgramCounterSymbolInfo Info;
            FPlatformStackWalk::ProgramCounterToSymbolInfo(ProgramCounter, Info);
            FResolvedSymbol Symbol;
            Symbol.Module = FString(ANSI_TO_TCHAR(Info.ModuleName));
            Symbol.Function = FString(ANSI_TO_TCHAR(Info.FunctionName));
            Symbol.File = FString(ANSI_TO_TCHAR(Info.Filename));
            Symbol.Line = Info.LineNumber;
            Entry.Symbols.Add(MoveTemp(Symbol));
        }
#else
        (void)ProgramCounter;
        (void)Result;
#endif
    }

    void SymbolizeAndWriteSummary(FSummary& Result)
    {
        TMap<uint64, FResolvedProgramCounter> SymbolCache;
        FProfileAggregateSet AllAggregates;
        FProfileAggregateSet Lod0Aggregates;
        FProfileAggregateSet Lod1PlusAggregates;
        TSet<FString> SeenFunctions;
        uint64 ResolvedFrames = 0;
        uint64 UnknownFrames = 0;
        uint64 WaitLikeFrames = 0;
        uint64 SamplesWithResolvedLeaf = 0;
        uint64 SamplesWithWaitLikeFrame = 0;
        uint64 SamplesWithStack = 0;
        uint64 UntaggedSamples = 0;

        auto AddUnknownExclusiveRow = [](FProfileAggregateSet& Aggregate)
        {
            const FString UnknownKey = TEXT("unknown\x1f<unknown>");
            FProfileRow& UnknownRow = Aggregate.ExclusiveFunctions.FindOrAdd(UnknownKey);
            if (UnknownRow.Key.IsEmpty())
            {
                UnknownRow.Key = UnknownKey;
                UnknownRow.Function = TEXT("<unknown>");
                UnknownRow.Module = TEXT("<unknown>");
                UnknownRow.File = TEXT("<unknown>");
                UnknownRow.Label = TEXT("unknown");
            }
            ++UnknownRow.Samples;
        };

        for (uint32 SampleIndex = 0; SampleIndex < StoredSampleCount; ++SampleIndex)
        {
            const FStoredSample& Sample = Samples[SampleIndex];
            if (Sample.Depth == 0)
            {
                continue;
            }
            ++SamplesWithStack;
            SeenFunctions.Reset();
            TArray<FResolvedSymbol> SampleFunctions;

            const FResolvedProgramCounter* LeafEntry = nullptr;
            const FResolvedSymbol* LeafFunction = nullptr;
            const FResolvedSymbol* LeafSource = nullptr;
            uint64 LeafProgramCounter = 0;
            bool bHasLeafProgramCounter = false;
            bool bSampleHasResolvedLeaf = false;
            bool bSampleHasWaitLike = false;

            for (uint32 FrameIndex = 0; FrameIndex < Sample.Depth; ++FrameIndex)
            {
                const uint64 ProgramCounter = Sample.ProgramCounters[FrameIndex];
                ResolveProgramCounter(ProgramCounter, SymbolCache, Result);
                const FResolvedProgramCounter* Entry = SymbolCache.Find(ProgramCounter);
                const FResolvedSymbol* FrameFunction = Entry != nullptr
                    ? FirstFunctionSymbol(*Entry) : nullptr;
                if (FrameFunction != nullptr)
                {
                    ++ResolvedFrames;
                    if (IsWaitLike(*FrameFunction))
                    {
                        ++WaitLikeFrames;
                        bSampleHasWaitLike = true;
                    }
                    if (FrameIndex == 0)
                    {
                        LeafProgramCounter = ProgramCounter;
                        bHasLeafProgramCounter = true;
                        bSampleHasResolvedLeaf = true;
                    }
                }
                else
                {
                    ++UnknownFrames;
                }

                if (Entry != nullptr)
                {
                    if (FrameIndex == 0)
                    {
                        LeafProgramCounter = ProgramCounter;
                        bHasLeafProgramCounter = true;
                    }
                    for (const FResolvedSymbol& Symbol : Entry->Symbols)
                    {
                        if (Symbol.Function.IsEmpty())
                        {
                            continue;
                        }
                        const FString Key = FunctionKey(Symbol);
                        if (!SeenFunctions.Contains(Key))
                        {
                            SeenFunctions.Add(Key);
                            SampleFunctions.Add(Symbol);
                        }
                    }
                }
            }

            // SymbolCache is a TMap. FindOrAdd while resolving later frames may rehash it, so
            // pointers captured during the loop are not stable. Re-find the leaf only after all
            // PCs for this sample have been inserted.
            if (bHasLeafProgramCounter)
            {
                LeafEntry = SymbolCache.Find(LeafProgramCounter);
                if (LeafEntry != nullptr)
                {
                    LeafFunction = FirstFunctionSymbol(*LeafEntry);
                    LeafSource = FirstSourceSymbol(*LeafEntry);
                }
            }

            auto AddToAggregate = [&](FProfileAggregateSet& Aggregate)
            {
                ++Aggregate.SamplesWithStack;
                if (bSampleHasResolvedLeaf)
                {
                    AddFunctionRow(Aggregate.ExclusiveFunctions, LeafFunction, 1);
                }
                else
                {
                    AddUnknownExclusiveRow(Aggregate);
                }
                for (const FResolvedSymbol& Symbol : SampleFunctions)
                {
                    AddFunctionRow(Aggregate.InclusiveFunctions, &Symbol, 1);
                }
                AddSourceRow(Aggregate.ExclusiveLines, LeafFunction, LeafSource);
            };

            AddToAggregate(AllAggregates);
            if (Sample.LODLevel == 0)
            {
                AddToAggregate(Lod0Aggregates);
            }
            else if (Sample.LODLevel >= 1)
            {
                AddToAggregate(Lod1PlusAggregates);
            }
            else
            {
                ++UntaggedSamples;
            }

            if (bSampleHasResolvedLeaf)
            {
                ++SamplesWithResolvedLeaf;
            }
            if (bSampleHasWaitLike)
            {
                ++SamplesWithWaitLikeFrame;
            }
        }

        Result.UniqueProgramCounters = SymbolCache.Num();
        Result.ResolvedFrames = ResolvedFrames;
        Result.UnknownFrames = UnknownFrames;
        Result.WaitLikeFrames = WaitLikeFrames;
        Result.SamplesWithResolvedLeaf = SamplesWithResolvedLeaf;
        Result.SamplesWithWaitLikeFrame = SamplesWithWaitLikeFrame;

        FString Summary;
        Summary += TEXT("VoxelForge in-process stack sampler\n");
        Summary += TEXT("==================================\n");
        Summary.Appendf(TEXT("run_label: %s\n"), *RunLabel);
        Summary.Appendf(TEXT("supported: %s\n"), Result.bSupported ? TEXT("yes") : TEXT("no"));
        Summary.Appendf(TEXT("stack_walking_initialized: %s\n"),
            Result.bStackWalkingInitialized ? TEXT("yes") : TEXT("no"));
        Summary.Appendf(TEXT("interval_us: %u\n"), IntervalUs);
        Summary.Appendf(TEXT("max_stack_depth: %u\n"), MaxStackDepth);
        Summary.Appendf(TEXT("max_stored_samples: %u\n"), MaxStoredSamples);
        Summary.Appendf(TEXT("sampler_ticks: %llu\n"),
            static_cast<unsigned long long>(SamplerTicks));
        Summary.Appendf(TEXT("capture_attempts: %llu\n"),
            static_cast<unsigned long long>(CaptureAttempts));
        Summary.Appendf(TEXT("registered_thread_samples: %llu\n"),
            static_cast<unsigned long long>(RegisteredThreadSamples));
        Summary.Appendf(TEXT("registration_race_drops: %llu\n"),
            static_cast<unsigned long long>(RegistrationRaceDrops));
        Summary.Appendf(TEXT("capture_failures: %llu\n"),
            static_cast<unsigned long long>(CaptureFailures));
        Summary.Appendf(TEXT("retained_samples: %llu\n"),
            static_cast<unsigned long long>(StoredSampleCount));
        Summary.Appendf(TEXT("dropped_samples_after_bound: %llu\n"),
            static_cast<unsigned long long>(DroppedSamples));
        Summary.Appendf(TEXT("samples_with_stack: %llu\n"),
            static_cast<unsigned long long>(SamplesWithStack));
        Summary.Appendf(TEXT("lod0_samples_with_stack: %llu\n"),
            static_cast<unsigned long long>(Lod0Aggregates.SamplesWithStack));
        Summary.Appendf(TEXT("lod1plus_samples_with_stack: %llu\n"),
            static_cast<unsigned long long>(Lod1PlusAggregates.SamplesWithStack));
        Summary.Appendf(TEXT("untagged_samples: %llu\n"),
            static_cast<unsigned long long>(UntaggedSamples));
        Summary.Appendf(TEXT("total_frames: %llu\n"),
            static_cast<unsigned long long>(TotalFrames));
        Summary.Appendf(TEXT("resolved_frames: %llu\n"),
            static_cast<unsigned long long>(ResolvedFrames));
        Summary.Appendf(TEXT("unknown_frames: %llu\n"),
            static_cast<unsigned long long>(UnknownFrames));
        Summary.Appendf(TEXT("wait_like_frames: %llu\n"),
            static_cast<unsigned long long>(WaitLikeFrames));
        Summary.Appendf(TEXT("samples_with_resolved_leaf: %llu\n"),
            static_cast<unsigned long long>(SamplesWithResolvedLeaf));
        Summary.Appendf(TEXT("samples_with_wait_like_frame: %llu\n"),
            static_cast<unsigned long long>(SamplesWithWaitLikeFrame));
        Summary.Appendf(TEXT("unique_program_counters: %llu\n"),
            static_cast<unsigned long long>(Result.UniqueProgramCounters));
        Summary.Appendf(TEXT("inline_symbols: %llu\n"),
            static_cast<unsigned long long>(Result.InlineSymbols));
        Summary.Appendf(TEXT("max_active_registered_threads: %llu\n"),
            static_cast<unsigned long long>(MaxActiveRegisteredThreads));
        Summary.Appendf(TEXT("run_seconds: %.6f\n"), Result.RunSeconds);
        Summary += TEXT("\nClosure uses stable registration snapshots. Top-table percentages use retained samples with a non-empty stack; the raw CSV includes zero-depth captures.\n");
        Summary.Appendf(TEXT("resolved_frame_fraction: %.6f\n"), TotalFrames > 0
            ? static_cast<double>(ResolvedFrames) / static_cast<double>(TotalFrames) : 0.0);
        Summary.Appendf(TEXT("unknown_frame_fraction: %.6f\n"), TotalFrames > 0
            ? static_cast<double>(UnknownFrames) / static_cast<double>(TotalFrames) : 0.0);
        Summary.Appendf(TEXT("wait_like_frame_fraction: %.6f\n"), TotalFrames > 0
            ? static_cast<double>(WaitLikeFrames) / static_cast<double>(TotalFrames) : 0.0);
        Summary.Appendf(TEXT("resolved_leaf_fraction: %.6f\n"), SamplesWithStack > 0
            ? static_cast<double>(SamplesWithResolvedLeaf) / static_cast<double>(SamplesWithStack) : 0.0);
        Summary.Appendf(TEXT("wait_like_sample_fraction: %.6f\n"), SamplesWithStack > 0
            ? static_cast<double>(SamplesWithWaitLikeFrame) / static_cast<double>(SamplesWithStack) : 0.0);
        Summary += TEXT("\nLabels: plugin = VoxelForge, engine = Unreal/Engine, std = std:: frames, other = other resolved modules, unknown = no function symbol. Inline symbol callbacks were requested through DbgHelp when available.\n");
        AppendRows(Summary, TEXT("Top 30 exclusive functions (leaf)"), AllAggregates.ExclusiveFunctions, SamplesWithStack, 30);
        AppendRows(Summary, TEXT("Top 30 inclusive functions (once per sample)"), AllAggregates.InclusiveFunctions, SamplesWithStack, 30);
        AppendRows(Summary, TEXT("Top 30 exclusive source lines (leaf source)"), AllAggregates.ExclusiveLines, SamplesWithStack, 30);
        Summary += TEXT("\nLOD0 tables (floor)\n");
        AppendRows(Summary, TEXT("LOD0 top 30 exclusive functions (leaf)"), Lod0Aggregates.ExclusiveFunctions, Lod0Aggregates.SamplesWithStack, 30);
        AppendRows(Summary, TEXT("LOD0 top 30 inclusive functions (once per sample)"), Lod0Aggregates.InclusiveFunctions, Lod0Aggregates.SamplesWithStack, 30);
        AppendRows(Summary, TEXT("LOD0 top 30 exclusive source lines (leaf source)"), Lod0Aggregates.ExclusiveLines, Lod0Aggregates.SamplesWithStack, 30);
        Summary += TEXT("\nLOD1+ tables (coarse/background)\n");
        AppendRows(Summary, TEXT("LOD1+ top 30 exclusive functions (leaf)"), Lod1PlusAggregates.ExclusiveFunctions, Lod1PlusAggregates.SamplesWithStack, 30);
        AppendRows(Summary, TEXT("LOD1+ top 30 inclusive functions (once per sample)"), Lod1PlusAggregates.InclusiveFunctions, Lod1PlusAggregates.SamplesWithStack, 30);
        AppendRows(Summary, TEXT("LOD1+ top 30 exclusive source lines (leaf source)"), Lod1PlusAggregates.ExclusiveLines, Lod1PlusAggregates.SamplesWithStack, 30);

        if (FFileHelper::SaveStringToFile(
                Summary,
                *SummaryPath,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            Result.bOutputWritten = true;
        }
    }
};

FVoxelStackSampler& FVoxelStackSampler::Get()
{
    static FVoxelStackSampler Instance;
    return Instance;
}

bool FVoxelStackSampler::Start(
    uint32 IntervalUs,
    const FString& OutputDirectory,
    const FString& RunLabel)
{
    FScopeLock LifecycleLock(&LifecycleMutex);
    if (Impl != nullptr)
    {
        return false;
    }

#if !PLATFORM_WINDOWS
    UE_LOG(LogTemp, Error,
        TEXT("[VoxelStackSampler] requested on a non-Windows target; no profile was started."));
    return false;
#else
    if (!IFileManager::Get().MakeDirectory(*OutputDirectory, true))
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelStackSampler] could not create '%s'."), *OutputDirectory);
        return false;
    }

    FImpl* NewImpl = new FImpl(*this, IntervalUs, OutputDirectory, RunLabel);
    if (!NewImpl->StartThread())
    {
        delete NewImpl;
        return false;
    }
    Impl = NewImpl;
    GVoxelStackSamplerEnabled.store(true, std::memory_order_release);
    UE_LOG(LogTemp, Display,
        TEXT("[VoxelStackSampler] started label=%s interval_us=%u raw=%s summary=%s"),
        *RunLabel, NewImpl->IntervalUs, *NewImpl->RawSamplesPath, *NewImpl->SummaryPath);
    return true;
#endif
}

FVoxelStackSampler::FSummary FVoxelStackSampler::StopAndWrite()
{
    GVoxelStackSamplerEnabled.store(false, std::memory_order_release);
    FScopeLock LifecycleLock(&LifecycleMutex);
    FSummary Result;
    if (Impl == nullptr)
    {
        return Result;
    }

    FImpl* StoppedImpl = Impl;
    StoppedImpl->StopThread();
    Result = StoppedImpl->Finalize();
    UE_LOG(LogTemp, Display,
        TEXT("[VoxelStackSampler] stopped label=%s ticks=%llu registered_samples=%llu "
             "retained=%llu dropped=%llu resolved_fraction=%.4f raw=%s summary=%s"),
        *Result.RunLabel,
        static_cast<unsigned long long>(Result.SamplerTicks),
        static_cast<unsigned long long>(Result.RegisteredThreadSamples),
        static_cast<unsigned long long>(Result.RetainedSamples),
        static_cast<unsigned long long>(Result.DroppedSamples),
        Result.TotalFrames > 0
            ? static_cast<double>(Result.ResolvedFrames) / static_cast<double>(Result.TotalFrames)
            : 0.0,
        *Result.RawSamplesPath,
        *Result.SummaryPath);
    delete StoppedImpl;
    Impl = nullptr;
    return Result;
}

void FVoxelStackSampler::Shutdown()
{
    StopAndWrite();
}

bool FVoxelStackSampler::IsRunning() const
{
    FScopeLock LifecycleLock(&LifecycleMutex);
    return Impl != nullptr;
}

bool FVoxelStackSampler::RegisterCurrentThread(int32 LODLevel)
{
    // Keep this as the first and only operation in the disabled fast path.  In particular, do not
    // touch the singleton or TLS until this relaxed load says a session is active.
    if (!GVoxelStackSamplerEnabled.load(std::memory_order_relaxed))
    {
        return false;
    }
    return Get().RegisterThreadInternal(LODLevel);
}

void FVoxelStackSampler::DeregisterCurrentThread()
{
    Get().DeregisterThreadInternal();
}

bool FVoxelStackSampler::IsEnabled()
{
    return GVoxelStackSamplerEnabled.load(std::memory_order_relaxed);
}

bool FVoxelStackSampler::RegisterThreadInternal(int32 LODLevel)
{
    FScopeLock LifecycleLock(&LifecycleMutex);
    return Impl != nullptr && Impl->RegisterThread(LODLevel);
}

void FVoxelStackSampler::DeregisterThreadInternal()
{
    FScopeLock LifecycleLock(&LifecycleMutex);
    if (GTlsRegistrationDepth <= 0)
    {
        ResetRegistrationTls();
        return;
    }

    if (Impl != nullptr
        && GTlsRegisteredImpl == Impl
        && GTlsSamplerSessionGeneration == Impl->SamplerSessionGeneration)
    {
        Impl->DeregisterThread();
        return;
    }

    // StopAndWrite may have detached and destroyed the session before a worker's scope guard
    // runs. Clear only this thread's stale registration; never dereference the old FImpl.
    ResetRegistrationTls();
}
