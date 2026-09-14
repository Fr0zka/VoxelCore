// VoxelStackOfflineSymbolizer.h

#pragma once

#include "CoreMinimal.h"

namespace VoxelForgeOfflineStackSymbolizer
{
    /** Resolve one sampler CSV after the measured process has exited. */
    bool Run(
        const FString& CsvPath,
        const FString& OptionalOutputPath,
        const FString& OptionalSymbolPath,
        FString& OutSummaryPath,
        FString& OutError);
}
