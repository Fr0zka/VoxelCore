// VoxelStackOfflineSymbolizer.cpp
// Post-exit PC -> module/RVA -> function/source/inline resolution.

#include "VoxelStackOfflineSymbolizer.h"

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Serialization/Archive.h"
#include "Containers/StringConv.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <Windows.h>
#include <DbgHelp.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
    void WriteUtf8(FArchive& Archive, const FString& Text)
    {
        FTCHARToUTF8 Utf8(*Text);
        if (Utf8.Length() > 0)
        {
            Archive.Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
        }
    }

    FString SanitizeField(const FString& InValue)
    {
        FString Result = InValue;
        Result.ReplaceInline(TEXT("\t"), TEXT(" "));
        Result.ReplaceInline(TEXT("\r"), TEXT(" "));
        Result.ReplaceInline(TEXT("\n"), TEXT(" "));
        return Result;
    }

    FString NormalizedForClassification(const FString& InValue)
    {
        FString Result = InValue;
        Result.ToLowerInline();
        Result.ReplaceInline(TEXT("\\"), TEXT("/"));
        return Result;
    }

    struct FOfflineModule
    {
        FString Path;
        FString PdbGuid;
        FString PdbPath;
        uint32 PdbAge = 0;
        uint64 LoadBase = 0;
        uint32 ImageSize = 0;
        uint64 FileSize = 0;
        uint64 LastWriteFileTime = 0;
        bool bNeeded = false;
        bool bLoaded = false;
        bool bCurrentIdentityMatches = true;
    };

    struct FOfflineSample
    {
        uint64 SampleIndex = 0;
        uint64 ElapsedUs = 0;
        uint64 ThreadId = 0;
        int32 LODLevel = INDEX_NONE;
        uint32 Depth = 0;
        uint64 ProgramCounters[64]{};
    };

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
        int32 ModuleIndex = INDEX_NONE;
        uint64 Rva = 0;
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
        if (FileLower.Contains(TEXT("/engine/")))
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

    FString DeriveSiblingPath(const FString& CsvPath, const TCHAR* OldToken, const TCHAR* NewToken,
                              const TCHAR* Extension)
    {
        const FString FullPath = FPaths::ConvertRelativePathToFull(CsvPath);
        FString BaseName = FPaths::GetBaseFilename(FullPath);
        if (BaseName.StartsWith(OldToken, ESearchCase::IgnoreCase))
        {
            BaseName = FString(NewToken) + BaseName.RightChop(FCString::Strlen(OldToken));
        }
        else
        {
            BaseName += FString(TEXT(".")) + NewToken;
        }
        return FPaths::Combine(FPaths::GetPath(FullPath), BaseName + Extension);
    }

    bool ParseUint64(const FString& Text, uint64& OutValue)
    {
        if (Text.IsEmpty())
        {
            return false;
        }
        TCHAR* End = nullptr;
        OutValue = FCString::Strtoui64(*Text, &End, 0);
        return End != nullptr && End != *Text;
    }

    bool ReadModuleMap(
        const FString& ModuleMapPath,
        TArray<FOfflineModule>& OutModules,
        FString& OutError)
    {
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *ModuleMapPath))
        {
            OutError = FString::Printf(TEXT("module map is not readable: %s"), *ModuleMapPath);
            return false;
        }

        OutModules.Reset();
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines, false);
        for (const FString& Line : Lines)
        {
            if (Line.IsEmpty() || Line.StartsWith(TEXT("#"))
                || Line.StartsWith(TEXT("module_path\t")))
            {
                continue;
            }

            TArray<FString> Fields;
            Line.ParseIntoArray(Fields, TEXT("\t"), false);
            if (Fields.Num() < 7)
            {
                continue;
            }

            FOfflineModule Module;
            Module.Path = Fields[0];
            uint64 ParsedImageSize = 0;
            uint64 ParsedPdbAge = 0;
            if (!ParseUint64(Fields[1], Module.LoadBase)
                || !ParseUint64(Fields[2], ParsedImageSize)
                || !ParseUint64(Fields[3], Module.FileSize)
                || !ParseUint64(Fields[4], Module.LastWriteFileTime)
                || !ParseUint64(Fields[6], ParsedPdbAge))
            {
                continue;
            }
            Module.ImageSize = static_cast<uint32>(ParsedImageSize);
            Module.PdbAge = static_cast<uint32>(ParsedPdbAge);
            Module.PdbGuid = Fields[5];
            Module.PdbPath = Fields.Num() > 7 ? Fields[7] : FString();
            OutModules.Add(MoveTemp(Module));
        }

        if (OutModules.Num() == 0)
        {
            OutError = FString::Printf(TEXT("module map has no module records: %s"), *ModuleMapPath);
            return false;
        }
        return true;
    }

#if PLATFORM_WINDOWS
    bool ReadCurrentFileIdentity(const FString& Path, uint64& OutSize, uint64& OutLastWrite)
    {
        WIN32_FILE_ATTRIBUTE_DATA FileData{};
        if (!GetFileAttributesExW(*Path, GetFileExInfoStandard, &FileData))
        {
            return false;
        }
        ULARGE_INTEGER Size;
        Size.HighPart = FileData.nFileSizeHigh;
        Size.LowPart = FileData.nFileSizeLow;
        OutSize = Size.QuadPart;
        ULARGE_INTEGER LastWrite;
        LastWrite.HighPart = FileData.ftLastWriteTime.dwHighDateTime;
        LastWrite.LowPart = FileData.ftLastWriteTime.dwLowDateTime;
        OutLastWrite = LastWrite.QuadPart;
        return true;
    }

    int32 FindModuleForPc(const TArray<FOfflineModule>& Modules, uint64 ProgramCounter)
    {
        for (int32 Index = 0; Index < Modules.Num(); ++Index)
        {
            const FOfflineModule& Module = Modules[Index];
            if (Module.ImageSize > 0 && ProgramCounter >= Module.LoadBase
                && ProgramCounter - Module.LoadBase < Module.ImageSize)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    FString DbgHelpFunctionName(const SYMBOL_INFO* Symbol, bool bInline)
    {
        if (Symbol == nullptr || Symbol->Name[0] == '\0')
        {
            return FString();
        }
        int32 Offset = 0;
        while (Symbol->Name[Offset] != '\0'
            && (Symbol->Name[Offset] < 32 || Symbol->Name[Offset] > 127))
        {
            ++Offset;
        }
        const FString Name = FString(ANSI_TO_TCHAR(Symbol->Name + Offset));
        return bInline
            ? FString::Printf(TEXT("[Inline Frame] %s()"), *Name)
            : FString::Printf(TEXT("%s()"), *Name);
    }

    void ResolveProgramCounter(
        HANDLE Process,
        uint64 ProgramCounter,
        const TArray<FOfflineModule>& Modules,
        TMap<uint64, FResolvedProgramCounter>& Cache,
        uint64& InOutInlineSymbols,
        TSet<uint64>& MappedPcs,
        TSet<uint64>& UnmappedPcs)
    {
        FResolvedProgramCounter& Entry = Cache.FindOrAdd(ProgramCounter);
        if (Entry.bAttempted)
        {
            return;
        }
        Entry.bAttempted = true;
        Entry.ModuleIndex = FindModuleForPc(Modules, ProgramCounter);
        if (Entry.ModuleIndex == INDEX_NONE)
        {
            UnmappedPcs.Add(ProgramCounter);
            return;
        }

        const FOfflineModule& Module = Modules[Entry.ModuleIndex];
        Entry.Rva = ProgramCounter - Module.LoadBase;
        MappedPcs.Add(ProgramCounter);
        const FString ModuleName = FPaths::GetCleanFilename(Module.Path);

        // The buffer shape and inline query match UE's Windows stack walker, but this resolver is
        // called only in the post-exit commandlet process.
        ANSICHAR SymbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        SYMBOL_INFO* Symbol = reinterpret_cast<SYMBOL_INFO*>(SymbolBuffer);
        Symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        Symbol->MaxNameLen = MAX_SYM_NAME;

        const DWORD InlineCount = SymAddrIncludeInlineTrace(Process, ProgramCounter);
        if (InlineCount > 0)
        {
            DWORD InlineContext = 0;
            DWORD FrameIndex = 0;
            if (SymQueryInlineTrace(
                    Process, ProgramCounter, 0, ProgramCounter, ProgramCounter,
                    &InlineContext, &FrameIndex))
            {
                for (DWORD Index = 0; Index < InlineCount; ++Index, ++InlineContext)
                {
                    DWORD64 Displacement = 0;
                    if (!SymFromInlineContext(
                            Process, ProgramCounter, InlineContext, &Displacement, Symbol))
                    {
                        continue;
                    }

                    FResolvedSymbol Resolved;
                    Resolved.Module = ModuleName;
                    Resolved.Function = DbgHelpFunctionName(Symbol, true);
                    Resolved.bInline = true;

                    IMAGEHLP_LINE64 Line{};
                    Line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
                    DWORD LineDisplacement = 0;
                    if (SymGetLineFromInlineContext(
                            Process, ProgramCounter, InlineContext, 0,
                            &LineDisplacement, &Line))
                    {
                        Resolved.File = Line.FileName != nullptr
                            ? FString(ANSI_TO_TCHAR(Line.FileName)) : FString();
                        Resolved.Line = static_cast<int32>(Line.LineNumber);
                    }
                    Entry.Symbols.Add(MoveTemp(Resolved));
                    ++InOutInlineSymbols;
                }
            }
        }

        DWORD64 Displacement = 0;
        if (SymFromAddr(Process, ProgramCounter, &Displacement, Symbol))
        {
            FResolvedSymbol Resolved;
            Resolved.Module = ModuleName;
            Resolved.Function = DbgHelpFunctionName(Symbol, false);
            IMAGEHLP_LINE64 Line{};
            Line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD LineDisplacement = 0;
            if (SymGetLineFromAddr64(Process, ProgramCounter, &LineDisplacement, &Line))
            {
                Resolved.File = Line.FileName != nullptr
                    ? FString(ANSI_TO_TCHAR(Line.FileName)) : FString();
                Resolved.Line = static_cast<int32>(Line.LineNumber);
            }
            Entry.Symbols.Add(MoveTemp(Resolved));
        }
    }

    void WritePcMap(
        const FString& PcMapPath,
        const TArray<FOfflineModule>& Modules,
        const TMap<uint64, FResolvedProgramCounter>& Cache)
    {
        FArchive* Archive = IFileManager::Get().CreateFileWriter(*PcMapPath);
        if (Archive == nullptr)
        {
            return;
        }
        WriteUtf8(*Archive, TEXT("# VoxelForge resolved PC map v1\n"));
        WriteUtf8(*Archive, TEXT("pc_hex\tmodule_path\tload_base_hex\trva_hex\tinline\tfunction\tfile\tline\n"));

        TArray<uint64> Pcs;
        Pcs.Reserve(Cache.Num());
        for (const TPair<uint64, FResolvedProgramCounter>& Pair : Cache)
        {
            Pcs.Add(Pair.Key);
        }
        Pcs.Sort();
        for (const uint64 Pc : Pcs)
        {
            const FResolvedProgramCounter* Entry = Cache.Find(Pc);
            if (Entry == nullptr)
            {
                continue;
            }
            const FOfflineModule* Module = Modules.IsValidIndex(Entry->ModuleIndex)
                ? &Modules[Entry->ModuleIndex] : nullptr;
            const FString ModulePath = Module != nullptr ? Module->Path : TEXT("<unmapped>");
            const uint64 LoadBase = Module != nullptr ? Module->LoadBase : 0;
            const FString ModuleLine = FString::Printf(
                TEXT("0x%016llX\t%s\t0x%016llX\t0x%llX\t"),
                static_cast<unsigned long long>(Pc),
                *SanitizeField(ModulePath),
                static_cast<unsigned long long>(LoadBase),
                static_cast<unsigned long long>(Entry->Rva));
            if (Entry->Symbols.Num() == 0)
            {
                WriteUtf8(*Archive, ModuleLine + TEXT("0\t<unknown>\t<unknown>\t0\n"));
                continue;
            }
            for (const FResolvedSymbol& Symbol : Entry->Symbols)
            {
                WriteUtf8(*Archive, ModuleLine + FString::Printf(
                    TEXT("%d\t%s\t%s\t%d\n"),
                    Symbol.bInline ? 1 : 0,
                    *SanitizeField(Symbol.Function),
                    *SanitizeField(Symbol.File),
                    Symbol.Line));
            }
        }
        delete Archive;
    }
#endif
}

namespace VoxelForgeOfflineStackSymbolizer
{
    bool Run(
        const FString& CsvPath,
        const FString& OptionalOutputPath,
        const FString& OptionalSymbolPath,
        FString& OutSummaryPath,
        FString& OutError)
    {
        OutSummaryPath.Reset();
        OutError.Reset();

#if !PLATFORM_WINDOWS
        OutError = TEXT("offline stack symbolization currently requires Windows DbgHelp");
        return false;
#else
        const FString FullCsvPath = FPaths::ConvertRelativePathToFull(CsvPath);
        if (!FPaths::FileExists(FullCsvPath))
        {
            OutError = FString::Printf(TEXT("CSV is not readable: %s"), *FullCsvPath);
            return false;
        }

        const FString ModuleMapPath = DeriveSiblingPath(
            FullCsvPath, TEXT("VoxelStackSamples_"), TEXT("VoxelStackModules_"), TEXT(".tsv"));
        const FString DefaultSummaryPath = DeriveSiblingPath(
            FullCsvPath, TEXT("VoxelStackSamples_"), TEXT("VoxelStackSummaryOffline_"), TEXT(".txt"));
        OutSummaryPath = OptionalOutputPath.IsEmpty()
            ? DefaultSummaryPath : FPaths::ConvertRelativePathToFull(OptionalOutputPath);
        const FString PcMapPath = DeriveSiblingPath(
            FullCsvPath, TEXT("VoxelStackSamples_"), TEXT("VoxelStackResolved_"), TEXT(".tsv"));

        TArray<FOfflineModule> Modules;
        if (!ReadModuleMap(ModuleMapPath, Modules, OutError))
        {
            return false;
        }

        for (FOfflineModule& Module : Modules)
        {
            uint64 CurrentSize = 0;
            uint64 CurrentLastWrite = 0;
            Module.bCurrentIdentityMatches =
                ReadCurrentFileIdentity(Module.Path, CurrentSize, CurrentLastWrite)
                && CurrentSize == Module.FileSize
                && CurrentLastWrite == Module.LastWriteFileTime;
        }

        FString CsvText;
        if (!FFileHelper::LoadFileToString(CsvText, *FullCsvPath))
        {
            OutError = FString::Printf(TEXT("CSV could not be loaded: %s"), *FullCsvPath);
            return false;
        }

        TArray<FOfflineSample> Samples;
        TArray<FString> Lines;
        CsvText.ParseIntoArrayLines(Lines, false);
        Samples.Reserve(Lines.Num());
        uint64 TotalFrames = 0;
        for (const FString& Line : Lines)
        {
            if (Line.IsEmpty() || Line.StartsWith(TEXT("sample_index,")))
            {
                continue;
            }
            TArray<FString> Fields;
            Line.ParseIntoArray(Fields, TEXT(","), false);
            if (Fields.Num() < 5)
            {
                continue;
            }

            FOfflineSample Sample;
            uint64 Parsed = 0;
            if (!ParseUint64(Fields[0], Sample.SampleIndex)
                || !ParseUint64(Fields[1], Sample.ElapsedUs)
                || !ParseUint64(Fields[2], Sample.ThreadId)
                || !ParseUint64(Fields[4], Parsed))
            {
                continue;
            }
            Sample.LODLevel = FCString::Atoi(*Fields[3]);
            Sample.Depth = static_cast<uint32>(FMath::Min<uint64>(
                Parsed, UE_ARRAY_COUNT(Sample.ProgramCounters)));
            for (uint32 FrameIndex = 0; FrameIndex < Sample.Depth; ++FrameIndex)
            {
                if (5 + FrameIndex >= static_cast<uint32>(Fields.Num()))
                {
                    Sample.Depth = FrameIndex;
                    break;
                }
                uint64 ProgramCounter = 0;
                if (!ParseUint64(Fields[5 + FrameIndex], ProgramCounter))
                {
                    Sample.Depth = FrameIndex;
                    break;
                }
                Sample.ProgramCounters[FrameIndex] = ProgramCounter;
            }
            TotalFrames += Sample.Depth;
            Samples.Add(MoveTemp(Sample));
        }

        HANDLE Process = GetCurrentProcess();
        const DWORD OldOptions = SymGetOptions();
        SymSetOptions(OldOptions
            | SYMOPT_LOAD_LINES
            | SYMOPT_UNDNAME
            | SYMOPT_DEFERRED_LOADS
            | SYMOPT_EXACT_SYMBOLS
            | SYMOPT_FAIL_CRITICAL_ERRORS);

        FString SearchPath = OptionalSymbolPath;
        for (const FOfflineModule& Module : Modules)
        {
            const FString ModuleDirectory = FPaths::GetPath(Module.Path);
            const FString PdbDirectory = FPaths::GetPath(Module.PdbPath);
            if (!ModuleDirectory.IsEmpty())
            {
                if (!SearchPath.IsEmpty()) SearchPath += TEXT(";");
                SearchPath += ModuleDirectory;
            }
            if (!PdbDirectory.IsEmpty() && !SearchPath.Contains(PdbDirectory))
            {
                if (!SearchPath.IsEmpty()) SearchPath += TEXT(";");
                SearchPath += PdbDirectory;
            }
        }

        if (!SymInitializeW(Process, *SearchPath, 0))
        {
            const DWORD Error = GetLastError();
            SymSetOptions(OldOptions);
            OutError = FString::Printf(TEXT("SymInitializeW failed with error %u"), Error);
            return false;
        }

        TSet<int32> NeededModuleIndices;
        for (const FOfflineSample& Sample : Samples)
        {
            for (uint32 FrameIndex = 0; FrameIndex < Sample.Depth; ++FrameIndex)
            {
                const int32 ModuleIndex = FindModuleForPc(Modules, Sample.ProgramCounters[FrameIndex]);
                if (ModuleIndex != INDEX_NONE)
                {
                    NeededModuleIndices.Add(ModuleIndex);
                }
            }
        }

        int32 LoadedModules = 0;
        int32 NeededModules = NeededModuleIndices.Num();
        for (FOfflineModule& Module : Modules)
        {
            Module.bNeeded = NeededModuleIndices.Contains(static_cast<int32>(&Module - Modules.GetData()));
            if (!Module.bNeeded) continue;
            const DWORD64 LoadedBase = SymLoadModuleExW(
                Process, nullptr, *Module.Path, *Module.Path,
                static_cast<DWORD64>(Module.LoadBase), Module.ImageSize, nullptr, 0);
            if (LoadedBase != 0)
            {
                Module.bLoaded = true;
                ++LoadedModules;
            }
        }

        TMap<uint64, FResolvedProgramCounter> SymbolCache;
        TSet<uint64> MappedPcs;
        TSet<uint64> UnmappedPcs;
        FProfileAggregateSet AllAggregates;
        FProfileAggregateSet Lod0Aggregates;
        FProfileAggregateSet Lod1PlusAggregates;
        uint64 ResolvedFrames = 0;
        uint64 UnknownFrames = 0;
        uint64 WaitLikeFrames = 0;
        uint64 SamplesWithResolvedLeaf = 0;
        uint64 SamplesWithWaitLikeFrame = 0;
        uint64 SamplesWithStack = 0;
        uint64 UntaggedSamples = 0;
        uint64 InlineSymbols = 0;

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

        for (const FOfflineSample& Sample : Samples)
        {
            if (Sample.Depth == 0)
            {
                continue;
            }
            ++SamplesWithStack;
            TSet<FString> SeenFunctions;
            TArray<FResolvedSymbol> SampleFunctions;
            uint64 LeafProgramCounter = 0;
            bool bHasLeafProgramCounter = false;
            bool bSampleHasResolvedLeaf = false;
            bool bSampleHasWaitLike = false;

            for (uint32 FrameIndex = 0; FrameIndex < Sample.Depth; ++FrameIndex)
            {
                const uint64 ProgramCounter = Sample.ProgramCounters[FrameIndex];
                ResolveProgramCounter(
                    Process, ProgramCounter, Modules, SymbolCache, InlineSymbols,
                    MappedPcs, UnmappedPcs);
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
                        bSampleHasResolvedLeaf = true;
                    }
                }
                else
                {
                    ++UnknownFrames;
                }

                if (FrameIndex == 0)
                {
                    LeafProgramCounter = ProgramCounter;
                    bHasLeafProgramCounter = true;
                }
                if (Entry != nullptr)
                {
                    for (const FResolvedSymbol& Symbol : Entry->Symbols)
                    {
                        if (Symbol.Function.IsEmpty()) continue;
                        const FString Key = FunctionKey(Symbol);
                        if (!SeenFunctions.Contains(Key))
                        {
                            SeenFunctions.Add(Key);
                            SampleFunctions.Add(Symbol);
                        }
                    }
                }
            }

            const FResolvedProgramCounter* LeafEntry = bHasLeafProgramCounter
                ? SymbolCache.Find(LeafProgramCounter) : nullptr;
            const FResolvedSymbol* LeafFunction = LeafEntry != nullptr
                ? FirstFunctionSymbol(*LeafEntry) : nullptr;
            const FResolvedSymbol* LeafSource = LeafEntry != nullptr
                ? FirstSourceSymbol(*LeafEntry) : nullptr;

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

            if (bSampleHasResolvedLeaf) ++SamplesWithResolvedLeaf;
            if (bSampleHasWaitLike) ++SamplesWithWaitLikeFrame;
        }

        const int32 IdentityMismatches = [&Modules]()
        {
            int32 Count = 0;
            for (const FOfflineModule& Module : Modules)
            {
                if (!Module.bCurrentIdentityMatches) ++Count;
            }
            return Count;
        }();

        WritePcMap(PcMapPath, Modules, SymbolCache);
        SymCleanup(Process);
        SymSetOptions(OldOptions);

        FString Summary;
        Summary += TEXT("VoxelForge offline stack symbolizer\n");
        Summary += TEXT("==================================\n");
        Summary.Appendf(TEXT("csv_path: %s\n"), *FullCsvPath);
        Summary.Appendf(TEXT("module_map_path: %s\n"), *ModuleMapPath);
        Summary.Appendf(TEXT("resolved_pc_map_path: %s\n"), *PcMapPath);
        Summary += TEXT("symbolization: offline\n");
        Summary.Appendf(TEXT("dbghelp_in_measured_process: no\n"));
        Summary.Appendf(TEXT("dbghelp_in_symbolizer_process: yes\n"));
        Summary.Appendf(TEXT("module_count: %d\n"), Modules.Num());
        Summary.Appendf(TEXT("needed_module_count: %d\n"), NeededModules);
        Summary.Appendf(TEXT("loaded_module_count: %d\n"), LoadedModules);
        Summary.Appendf(TEXT("module_identity_mismatches: %d\n"), IdentityMismatches);
        Summary.Appendf(TEXT("samples: %d\n"), Samples.Num());
        Summary.Appendf(TEXT("samples_with_stack: %llu\n"), static_cast<unsigned long long>(SamplesWithStack));
        Summary.Appendf(TEXT("lod0_samples_with_stack: %llu\n"),
            static_cast<unsigned long long>(Lod0Aggregates.SamplesWithStack));
        Summary.Appendf(TEXT("lod1plus_samples_with_stack: %llu\n"),
            static_cast<unsigned long long>(Lod1PlusAggregates.SamplesWithStack));
        Summary.Appendf(TEXT("untagged_samples: %llu\n"), static_cast<unsigned long long>(UntaggedSamples));
        Summary.Appendf(TEXT("total_frames: %llu\n"), static_cast<unsigned long long>(TotalFrames));
        Summary.Appendf(TEXT("resolved_frames: %llu\n"), static_cast<unsigned long long>(ResolvedFrames));
        Summary.Appendf(TEXT("unknown_frames: %llu\n"), static_cast<unsigned long long>(UnknownFrames));
        Summary.Appendf(TEXT("wait_like_frames: %llu\n"), static_cast<unsigned long long>(WaitLikeFrames));
        Summary.Appendf(TEXT("samples_with_resolved_leaf: %llu\n"),
            static_cast<unsigned long long>(SamplesWithResolvedLeaf));
        Summary.Appendf(TEXT("samples_with_wait_like_frame: %llu\n"),
            static_cast<unsigned long long>(SamplesWithWaitLikeFrame));
        Summary.Appendf(TEXT("unique_program_counters: %d\n"), SymbolCache.Num());
        Summary.Appendf(TEXT("mapped_program_counters: %d\n"), MappedPcs.Num());
        Summary.Appendf(TEXT("unmapped_program_counters: %d\n"), UnmappedPcs.Num());
        Summary.Appendf(TEXT("inline_symbols: %llu\n"), static_cast<unsigned long long>(InlineSymbols));
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
        Summary += TEXT("\nLabels: plugin = VoxelForge, engine = Unreal/Engine, std = std:: frames, other = other resolved modules, unknown = no function symbol. Inline callbacks are resolved only in this offline process.\n");
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

        IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutSummaryPath), true);
        if (!FFileHelper::SaveStringToFile(
                Summary, *OutSummaryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            OutError = FString::Printf(TEXT("could not write offline summary: %s"), *OutSummaryPath);
            return false;
        }
        return true;
#endif
    }
}
