// VoxelStackSampler.cpp
// Fixed-storage raw stack sampling for VoxelForge generation workers.

#include "VoxelStackSampler.h"

#include "Containers/Array.h"
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
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/Archive.h"
#include "Templates/UniquePtr.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsPlatformStackWalk.h"
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <Psapi.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
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

#if PLATFORM_WINDOWS
struct FModuleMapRecord
{
    FString Path;
    uint64 LoadBase = 0;
    uint32 ImageSize = 0;
    uint64 FileSize = 0;
    uint64 LastWriteFileTime = 0;
    FString PdbGuid;
    uint32 PdbAge = 0;
    FString PdbPath;
};

FString ModuleMapField(const FString& InValue)
{
    FString Result = InValue;
    Result.ReplaceInline(TEXT("\t"), TEXT(" "));
    Result.ReplaceInline(TEXT("\r"), TEXT(" "));
    Result.ReplaceInline(TEXT("\n"), TEXT(" "));
    return Result;
}

bool IsModuleImageRangeValid(uint32 Offset, uint64 Size, uint32 ImageSize)
{
    return Offset <= ImageSize && Size <= static_cast<uint64>(ImageSize - Offset);
}

void ReadModuleCodeViewIdentity(
    FModuleMapRecord& Record, const uint8* ModuleBase, uint32 ImageSize)
{
    if (ModuleBase == nullptr || ImageSize < sizeof(IMAGE_DOS_HEADER))
    {
        return;
    }

    const IMAGE_DOS_HEADER* DosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(ModuleBase);
    if (DosHeader->e_magic != IMAGE_DOS_SIGNATURE || DosHeader->e_lfanew < 0)
    {
        return;
    }

    const uint32 NtOffset = static_cast<uint32>(DosHeader->e_lfanew);
    if (!IsModuleImageRangeValid(NtOffset, sizeof(IMAGE_NT_HEADERS), ImageSize))
    {
        return;
    }
    const IMAGE_NT_HEADERS* NtHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        ModuleBase + NtOffset);
    if (NtHeaders->Signature != IMAGE_NT_SIGNATURE
        || NtHeaders->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
    {
        return;
    }

    const IMAGE_DATA_DIRECTORY& DebugDirectory =
        NtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (DebugDirectory.VirtualAddress == 0
        || DebugDirectory.Size < sizeof(IMAGE_DEBUG_DIRECTORY)
        || !IsModuleImageRangeValid(
            DebugDirectory.VirtualAddress, DebugDirectory.Size, ImageSize))
    {
        return;
    }

    const IMAGE_DEBUG_DIRECTORY* Entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(
        ModuleBase + DebugDirectory.VirtualAddress);
    const uint32 EntryCount = DebugDirectory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (uint32 Index = 0; Index < EntryCount; ++Index)
    {
        const IMAGE_DEBUG_DIRECTORY& Entry = Entries[Index];
        if (Entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || Entry.SizeOfData < 24
            || Entry.AddressOfRawData == 0
            || !IsModuleImageRangeValid(
                Entry.AddressOfRawData, Entry.SizeOfData, ImageSize))
        {
            continue;
        }

        const uint8* CodeView = ModuleBase + Entry.AddressOfRawData;
        if (CodeView[0] != 'R' || CodeView[1] != 'S'
            || CodeView[2] != 'D' || CodeView[3] != 'S')
        {
            continue;
        }

        FGuid Guid;
        FMemory::Memcpy(&Guid, CodeView + 4, sizeof(Guid));
        Record.PdbGuid = Guid.ToString();
        FMemory::Memcpy(&Record.PdbAge, CodeView + 20, sizeof(Record.PdbAge));

        const uint32 PdbPathBytes = Entry.SizeOfData - 24;
        if (PdbPathBytes > 0)
        {
            const ANSICHAR* PdbPathAnsi = reinterpret_cast<const ANSICHAR*>(CodeView + 24);
            int32 Length = 0;
            while (Length < static_cast<int32>(PdbPathBytes) && PdbPathAnsi[Length] != '\0')
            {
                ++Length;
            }
            if (Length > 0)
            {
                FUTF8ToTCHAR PdbPathUtf8(PdbPathAnsi, Length);
                Record.PdbPath = FString(PdbPathUtf8.Get());
            }
        }
        return;
    }
}

bool CollectProcessModuleMap(TArray<FModuleMapRecord>& OutRecords, FString& OutError)
{
    OutRecords.Reset();
    OutError.Reset();

    // Keep PSAPI dynamic, like the engine stack-walk implementation. This records module state
    // without touching DbgHelp or the process-global symbol engine.
    void* PsapiHandle = FPlatformProcess::GetDllHandle(TEXT("PSAPI.DLL"));
    if (PsapiHandle == nullptr)
    {
        OutError = TEXT("PSAPI.DLL could not be loaded");
        return false;
    }

    using FEnumProcessModules = BOOL (WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
    using FGetModuleFileNameExW = DWORD (WINAPI*)(HANDLE, HMODULE, LPWSTR, DWORD);
    using FGetModuleInformation = BOOL (WINAPI*)(HANDLE, HMODULE, LPMODULEINFO, DWORD);

    const FEnumProcessModules EnumModules = reinterpret_cast<FEnumProcessModules>(
        FPlatformProcess::GetDllExport(PsapiHandle, TEXT("EnumProcessModules")));
    const FGetModuleFileNameExW GetModuleFileNameEx = reinterpret_cast<FGetModuleFileNameExW>(
        FPlatformProcess::GetDllExport(PsapiHandle, TEXT("GetModuleFileNameExW")));
    const FGetModuleInformation GetModuleInformation = reinterpret_cast<FGetModuleInformation>(
        FPlatformProcess::GetDllExport(PsapiHandle, TEXT("GetModuleInformation")));
    if (EnumModules == nullptr || GetModuleFileNameEx == nullptr || GetModuleInformation == nullptr)
    {
        OutError = TEXT("PSAPI exports needed for module enumeration are unavailable");
        FPlatformProcess::FreeDllHandle(PsapiHandle);
        return false;
    }

    DWORD BytesRequired = 0;
    EnumModules(GetCurrentProcess(), nullptr, 0, &BytesRequired);
    if (BytesRequired == 0)
    {
        OutError = TEXT("EnumProcessModules returned no module bytes");
        FPlatformProcess::FreeDllHandle(PsapiHandle);
        return false;
    }

    TArray<HMODULE> Modules;
    Modules.SetNumUninitialized(
        FMath::Max<int32>(1, static_cast<int32>(BytesRequired / sizeof(HMODULE) + 1)));
    DWORD BytesWritten = 0;
    if (!EnumModules(
            GetCurrentProcess(), Modules.GetData(),
            static_cast<DWORD>(Modules.Num() * sizeof(HMODULE)), &BytesWritten))
    {
        OutError = TEXT("EnumProcessModules failed");
        FPlatformProcess::FreeDllHandle(PsapiHandle);
        return false;
    }

    const int32 ModuleCount = FMath::Min<int32>(
        Modules.Num(), static_cast<int32>(BytesWritten / sizeof(HMODULE)));
    for (int32 Index = 0; Index < ModuleCount; ++Index)
    {
        MODULEINFO ModuleInfo{};
        if (!GetModuleInformation(
                GetCurrentProcess(), Modules[Index], &ModuleInfo, sizeof(ModuleInfo)))
        {
            continue;
        }

        WCHAR ImagePath[32768] = {};
        const DWORD PathLength = GetModuleFileNameEx(
            GetCurrentProcess(), Modules[Index], ImagePath, UE_ARRAY_COUNT(ImagePath));
        if (PathLength == 0)
        {
            continue;
        }

        FModuleMapRecord Record;
        Record.Path = FString(ImagePath, static_cast<int32>(PathLength));
        Record.LoadBase = reinterpret_cast<uint64>(ModuleInfo.lpBaseOfDll);
        Record.ImageSize = static_cast<uint32>(ModuleInfo.SizeOfImage);

        WIN32_FILE_ATTRIBUTE_DATA FileData{};
        if (GetFileAttributesExW(*Record.Path, GetFileExInfoStandard, &FileData))
        {
            ULARGE_INTEGER FileSize;
            FileSize.HighPart = FileData.nFileSizeHigh;
            FileSize.LowPart = FileData.nFileSizeLow;
            Record.FileSize = FileSize.QuadPart;
            ULARGE_INTEGER LastWrite;
            LastWrite.HighPart = FileData.ftLastWriteTime.dwHighDateTime;
            LastWrite.LowPart = FileData.ftLastWriteTime.dwLowDateTime;
            Record.LastWriteFileTime = LastWrite.QuadPart;
        }

        ReadModuleCodeViewIdentity(
            Record, reinterpret_cast<const uint8*>(ModuleInfo.lpBaseOfDll), Record.ImageSize);
        OutRecords.Add(MoveTemp(Record));
    }

    OutRecords.Sort([](const FModuleMapRecord& A, const FModuleMapRecord& B)
    {
        return A.LoadBase < B.LoadBase;
    });
    FPlatformProcess::FreeDllHandle(PsapiHandle);
    return OutRecords.Num() > 0;
}
#endif

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
    FString ModuleMapPath;
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
        ModuleMapPath = FPaths::Combine(
            OutputDirectory,
            FString::Printf(TEXT("VoxelStackModules_%s_%u.tsv"), *SafeLabel, ProcessId));
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
        Result.ModuleMapPath = ModuleMapPath;
        Result.SummaryPath = SummaryPath;
        Result.RunSeconds = FPlatformTime::Seconds() - StartSeconds;

        WriteRawSamples(Result);
        WriteModuleMap(Result);
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

    void WriteModuleMap(FSummary& Result)
    {
#if PLATFORM_WINDOWS
        TArray<FModuleMapRecord> Records;
        FString CollectionError;
        const bool bCollected = CollectProcessModuleMap(Records, CollectionError);

        FArchive* Archive = IFileManager::Get().CreateFileWriter(*ModuleMapPath);
        if (Archive == nullptr)
        {
            return;
        }

        WriteUtf8(*Archive, TEXT("# VoxelForge module map v1\n"));
        WriteUtf8(*Archive, TEXT("# DbgHelp is not initialized by this artifact writer.\n"));
        WriteUtf8(*Archive, TEXT("module_path\tload_base_hex\timage_size\tfile_size\tfile_last_write_filetime\tpdb_guid\tpdb_age\tpdb_path\n"));
        if (!bCollected)
        {
            WriteUtf8(*Archive, FString::Printf(TEXT("# collection_error\t%s\n"), *CollectionError));
        }
        for (const FModuleMapRecord& Record : Records)
        {
            const FString Line = FString::Printf(
                TEXT("%s\t0x%016llX\t%u\t%llu\t%llu\t%s\t%u\t%s\n"),
                *ModuleMapField(Record.Path),
                static_cast<unsigned long long>(Record.LoadBase),
                Record.ImageSize,
                static_cast<unsigned long long>(Record.FileSize),
                static_cast<unsigned long long>(Record.LastWriteFileTime),
                *ModuleMapField(Record.PdbGuid),
                Record.PdbAge,
                *ModuleMapField(Record.PdbPath));
            WriteUtf8(*Archive, Line);
        }
        delete Archive;
        Result.ModuleCount = static_cast<uint64>(Records.Num());
        Result.bModuleMapWritten = true;
#else
        (void)Result;
#endif
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
        Summary.Appendf(TEXT("module_map_written: %s\n"),
            Result.bModuleMapWritten ? TEXT("yes") : TEXT("no"));
        Summary.Appendf(TEXT("module_count: %llu\n"),
            static_cast<unsigned long long>(Result.ModuleCount));
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
        Summary.Appendf(TEXT("module_map_path: %s\n"), *ModuleMapPath);
        Summary += TEXT("\nThe target process records bounded raw PCs only; resolve symbols after the process exits.\n");

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
        TEXT("[VoxelStackSampler] started label=%s interval_us=%u raw=%s modules=%s summary=%s"),
        *RunLabel, NewImpl->IntervalUs, *NewImpl->RawSamplesPath,
        *NewImpl->ModuleMapPath, *NewImpl->SummaryPath);
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
             "retained=%llu dropped=%llu raw=%s module_map=%s summary=%s"),
        *Result.RunLabel,
        static_cast<unsigned long long>(Result.SamplerTicks),
        static_cast<unsigned long long>(Result.RegisteredThreadSamples),
        static_cast<unsigned long long>(Result.RetainedSamples),
        static_cast<unsigned long long>(Result.DroppedSamples),
        *Result.RawSamplesPath,
        *Result.ModuleMapPath,
        *Result.SummaryPath);
    delete StoppedImpl;
    Impl = nullptr;
    return Result;
}

void FVoxelStackSampler::Shutdown()
{
    StopAndWrite();
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
