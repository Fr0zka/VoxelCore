// VoxelStackSampler.h
// Opt-in raw stack sampling for active VoxelForge generation workers; symbols are resolved offline.

#pragma once

#include "CoreTypes.h"
#include "Containers/UnrealString.h"
#include "HAL/CriticalSection.h"

/**
 * A diagnostic-only raw stack sampler.
 *
 * The generation path only constructs FScopedVoxelStackRegistration at task entry.  When the
 * sampler is disabled that constructor performs one relaxed load of the global enable flag and
 * returns; it does not touch the registry, allocate, lock, or read a console variable.
 *
 * The sampler thread owns a bounded preallocated array of raw program counters.  The measured
 * process never initializes or calls DbgHelp: the raw CSV is the durable sampling artifact and
 * symbolization is explicitly deferred to an offline consumer.
 */
class VOXELFORGE_API FVoxelStackSampler
{
public:
    static constexpr uint32 DefaultIntervalUs = 1000;
    static constexpr uint32 MinIntervalUs = 1;
    static constexpr uint32 MaxIntervalUs = 1000000;
    static constexpr uint32 MaxStackDepth = 64;
    // Bounded allocation. The implementation uses reservoir replacement after this fills so a
    // long moving session is represented across its whole run rather than only its first window.
    static constexpr uint32 MaxStoredSamples = 131072;

    struct FSummary
    {
        bool bSupported = false;
        bool bStarted = false;
        bool bOutputWritten = false;
        bool bModuleMapWritten = false;
        bool bStackWalkingInitialized = false;
        uint32 IntervalUs = 0;
        uint32 MaxDepth = 0;
        uint32 MaxStoredSampleCount = 0;

        uint64 SamplerTicks = 0;
        uint64 CaptureAttempts = 0;
        uint64 RegisteredThreadSamples = 0;
        uint64 RegistrationRaceDrops = 0;
        uint64 CaptureFailures = 0;
        uint64 RetainedSamples = 0;
        uint64 DroppedSamples = 0;
        uint64 TotalFrames = 0;
        uint64 MaxActiveRegisteredThreads = 0;
        uint64 ModuleCount = 0;
        double RunSeconds = 0.0;

        FString RunLabel;
        FString RawSamplesPath;
        FString ModuleMapPath;
        FString SummaryPath;
    };

    static FVoxelStackSampler& Get();

    /** Start one sampling session. Returns false when another session is active or setup fails. */
    bool Start(uint32 IntervalUs, const FString& OutputDirectory, const FString& RunLabel);

    /** Stop and write the bounded raw CSV plus a symbolization-free readable summary. */
    FSummary StopAndWrite();

    /** Used by the module shutdown path as a final lifecycle backstop. */
    void Shutdown();

    /** Called only by FScopedVoxelStackRegistration; cheap when disabled. */
    static bool RegisterCurrentThread(int32 LODLevel = INDEX_NONE);
    static void DeregisterCurrentThread();
    static bool IsEnabled();

private:
    FVoxelStackSampler() = default;
    ~FVoxelStackSampler() = default;
    FVoxelStackSampler(const FVoxelStackSampler&) = delete;
    FVoxelStackSampler& operator=(const FVoxelStackSampler&) = delete;

    struct FImpl;
    FImpl* Impl = nullptr;
    mutable FCriticalSection LifecycleMutex;

    bool RegisterThreadInternal(int32 LODLevel);
    void DeregisterThreadInternal();
};

/** Scope a generation task, not an individual density sample. */
class FScopedVoxelStackRegistration final
{
public:
    explicit FScopedVoxelStackRegistration(int32 InLODLevel = INDEX_NONE)
        : bRegistered(FVoxelStackSampler::RegisterCurrentThread(InLODLevel))
    {
    }

    ~FScopedVoxelStackRegistration()
    {
        if (bRegistered)
        {
            FVoxelStackSampler::DeregisterCurrentThread();
        }
    }

    FScopedVoxelStackRegistration(const FScopedVoxelStackRegistration&) = delete;
    FScopedVoxelStackRegistration& operator=(const FScopedVoxelStackRegistration&) = delete;

private:
    bool bRegistered = false;
};
