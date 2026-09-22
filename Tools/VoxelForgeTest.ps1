[CmdletBinding()]
param(
    [ValidateSet('canonical', 'owner', 'probe', 'perf', 'tests', 'surface-fall', 'crossing', 'gate-stress', 'parity')]
    [string]$Scenario = 'canonical',
    [switch]$Build,
    [ValidateSet('owner', 'default')]
    [string]$Assets = 'owner',
    [hashtable]$Cvars = @{},
    [string]$TestFilter,
    [string]$Out,
    [string]$Label
)

# One guarded entry point for all VoxelForge measurements.  It intentionally keeps the Unreal
# command lines in this file: callers choose a scenario and data, never an arbitrary process.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptFile = [IO.Path]::GetFullPath($PSCommandPath)
$ToolsRoot = Split-Path -Parent $ScriptFile
$PluginRoot = Split-Path -Parent $ToolsRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$HostRoot = Join-Path $PluginRoot 'Saved\BuildHost\HostProject'
$HostProject = Join-Path $HostRoot 'HostProject.uproject'
$StagePlugin = Join-Path $HostRoot 'Plugins\VoxelForge'
$ProjectFile = Join-Path $ProjectRoot 'VoxelM.uproject'
$EngineRoot = 'E:\Program Files\Epic Games\UE_5.7'
$EditorExe = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$UbtDll = Join-Path $EngineRoot 'Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll'
$LaunchTimeoutSeconds = 1800
# The VoxelForge category includes two late, several-minute op-stack sweeps. They remain in
# the suite; when the serialized aggregate launch reaches this 30-minute guard, validate those
# named tests with -TestFilter so every test still runs under the standing launch limit.
# Fresh branch-tip baseline at 1c642fd1824a7e0e4f88a8fc1f42b9e23fdb1821 after a clean staged
# sync (synthetic canonical, walk+export+probe). This is intentionally asserted so any
# field-affecting edit makes canonical fail loudly.
$CanonicalBaselineCommit = '1c642fd1824a7e0e4f88a8fc1f42b9e23fdb1821'
$CanonicalObjSha256 = '1280524041AB4932E6CFE74D57C349F84C6557C02CB7958C694DE734EF347E2B'

$Failures = [System.Collections.Generic.List[string]]::new()
$LaunchLedger = [System.Collections.Generic.List[object]]::new()
$BuildRecord = [ordered]@{ requested = [bool]$Build; status = 'not_requested' }
$AssetEvidence = @()
$CommandletMetrics = [ordered]@{}
$GameMetrics = [ordered]@{}
$SurfaceFallMetrics = [ordered]@{
    requested = $Scenario -eq 'surface-fall' -or
        ($Scenario -eq 'tests' -and [string]::IsNullOrWhiteSpace($TestFilter))
    status = 'not_run'
}
$StrateCrossingMetrics = [ordered]@{ requested = $Scenario -eq 'crossing'; status = 'not_run' }
$CollisionGateStressMetrics = [ordered]@{
    requested = $Scenario -eq 'gate-stress' -or
        ($Scenario -eq 'tests' -and [string]::IsNullOrWhiteSpace($TestFilter))
    status = 'not_run'
}
$TestMetrics = @()
$ParityEvidence = [ordered]@{ requested = $Scenario -eq 'parity'; status = 'not_run' }
$DllEvidence = [ordered]@{
    runtime = [ordered]@{ path = Join-Path $StagePlugin 'Binaries\Win64\UnrealEditor-VoxelForge.dll'; sha256 = $null }
    editor = [ordered]@{ path = Join-Path $StagePlugin 'Binaries\Win64\UnrealEditor-VoxelForgeEditor.dll'; sha256 = $null }
}
$LocalAppDataWrites = @()
$LocalAppDataRoot = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'UnrealEngine'
$RunRoot = $null
$ResultPath = $null
$SummaryPath = $null

function Add-Failure([string]$Reason) {
    if ([string]::IsNullOrWhiteSpace($Reason)) { return }
    if (-not $Failures.Contains($Reason)) { [void]$Failures.Add($Reason) }
}

function Ensure-Directory([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'Cannot create an empty directory path.' }
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        [IO.Directory]::CreateDirectory($Path) | Out-Null
    }
}

function Get-FullPath([string]$Path) {
    return [IO.Path]::GetFullPath($Path)
}

function Test-UnderPath([string]$Candidate, [string]$Root) {
    $candidateFull = (Get-FullPath $Candidate).TrimEnd('\', '/')
    $rootFull = (Get-FullPath $Root).TrimEnd('\', '/')
    return $candidateFull.Equals($rootFull, [StringComparison]::OrdinalIgnoreCase) -or
        $candidateFull.StartsWith($rootFull + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $candidateFull.StartsWith($rootFull + '/', [StringComparison]::OrdinalIgnoreCase)
}

function Get-Sha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToUpperInvariant()
}

function Get-SourceSnapshot([string]$Root) {
    $snapshot = [ordered]@{}
    $sourceRoot = Join-Path $Root 'Source'
    if (Test-Path -LiteralPath $sourceRoot -PathType Container) {
        foreach ($file in @(Get-ChildItem -LiteralPath $sourceRoot -File -Recurse -Force)) {
            $relative = $file.FullName.Substring($Root.Length).TrimStart('\', '/')
            $snapshot[$relative.ToLowerInvariant()] = Get-Sha256 $file.FullName
        }
    }
    $manifest = Join-Path $Root 'VoxelForge.uplugin'
    if (Test-Path -LiteralPath $manifest -PathType Leaf) {
        $snapshot['voxelforge.uplugin'] = Get-Sha256 $manifest
    }
    return $snapshot
}

function Get-ChangedSnapshotKeys($Before, $After) {
    $keys = @($Before.Keys) + @($After.Keys) | Sort-Object -Unique
    $changed = [System.Collections.Generic.List[string]]::new()
    foreach ($key in $keys) {
        if (-not $Before.Contains($key) -or -not $After.Contains($key) -or
            $Before[$key] -ne $After[$key]) {
            [void]$changed.Add([string]$key)
        }
    }
    return @($changed)
}

function Get-StateSnapshot([string]$Root) {
    $snapshot = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { return $snapshot }
    foreach ($item in @(Get-ChildItem -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue)) {
        $relative = $item.FullName.Substring($Root.Length).TrimStart('\', '/').ToLowerInvariant()
        $kind = if ($item.PSIsContainer) { 'D' } else { 'F' }
        $time = $item.LastWriteTimeUtc.Ticks
        $length = if ($item.PSIsContainer) { 0 } else { $item.Length }
        $snapshot["$kind|$relative"] = "$time|$length"
    }
    return $snapshot
}

function Get-StateChanges($Before, $After) {
    $keys = @($Before.Keys) + @($After.Keys) | Sort-Object -Unique
    $changed = [System.Collections.Generic.List[string]]::new()
    foreach ($key in $keys) {
        if (-not $Before.Contains($key) -or -not $After.Contains($key) -or $Before[$key] -ne $After[$key]) {
            [void]$changed.Add([string]$key)
        }
    }
    return @($changed)
}

function Get-JsonValue($Object, [string]$Name, $Default = $null) {
    if ($null -eq $Object) { return $Default }
    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $Default
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $Default }
    return $property.Value
}

function ConvertTo-CompactJson($Object) {
    if ($null -eq $Object) { return 'null' }
    return ($Object | ConvertTo-Json -Compress -Depth 30)
}

function Get-UnrealProcesses {
    # Get-Process is deliberately the primary guard. Some managed shells deny Win32_Process
    # command-line inspection, but they still expose the PID/name needed to protect an owner
    # editor before a launch.
    $rows = @(Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -like 'UnrealEditor*' } |
        ForEach-Object {
            [pscustomobject]@{
                ProcessId = [int]$_.Id
                Name = "$($_.ProcessName).exe"
                ExecutablePath = $null
                CommandLine = $null
            }
        })
    return @($rows | ForEach-Object {
        [pscustomobject]@{
            pid = [int]$_.ProcessId
            name = [string]$_.Name
            executable = [string]$_.ExecutablePath
            command_line = [string]$_.CommandLine
        }
    })
}

function Assert-NoForeignUnreal([string]$When) {
    $processes = @(Get-UnrealProcesses)
    if ($processes.Count -gt 0) {
        $detail = ($processes | ForEach-Object {
            "PID=$($_.pid) Name=$($_.name) Exe=$($_.executable)"
        }) -join '; '
        throw "UnrealEditor process already exists $When; refusing to touch it: $detail"
    }
}

function Get-CrashSnapshot {
    $snapshot = [ordered]@{}
    $roots = @(
        (Join-Path $HostRoot 'Saved\Crashes'),
        (Join-Path $StagePlugin 'Saved\Crashes'),
        (Join-Path $RunRoot 'Crashes')
    )
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        foreach ($dir in @(Get-ChildItem -LiteralPath $root -Directory -Filter 'UECC-*' -Recurse -Force -ErrorAction SilentlyContinue)) {
            $snapshot[$dir.FullName.ToLowerInvariant()] = $dir.LastWriteTimeUtc.Ticks
        }
    }
    return $snapshot
}

function Get-CrashReporterProcesses {
    $rows = @()
    try {
        $rows = @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop 2>$null |
            Where-Object { $_.Name -like '*CrashReportClient*' })
    } catch {
        # Without a parent/command line the process cannot be proven to be ours, so leave it
        # untouched. The ledger records the absence of a safely attributable reporter.
        $Error.Clear()
        return @()
    }
    return @($rows | ForEach-Object {
        [pscustomobject]@{
            pid = [int]$_.ProcessId
            name = [string]$_.Name
            command_line = [string]$_.CommandLine
        }
    })
}

function Reap-OwnCrashReporters($Before, [datetime]$StartedUtc, [string]$LogPath) {
    $beforePids = @($Before | ForEach-Object { [int]$_.pid })
    $reaped = [System.Collections.Generic.List[int]]::new()
    foreach ($reporter in @(Get-CrashReporterProcesses)) {
        if ($beforePids -contains [int]$reporter.pid) { continue }
        $commandLine = [string]$reporter.command_line
        $belongsToRun = $commandLine.IndexOf($RunRoot, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $commandLine.IndexOf($HostProject, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $commandLine.IndexOf($LogPath, [StringComparison]::OrdinalIgnoreCase) -ge 0
        if (-not $belongsToRun) { continue }
        try {
            Stop-Process -Id ([int]$reporter.pid) -Force -ErrorAction Stop
            [void]$reaped.Add([int]$reporter.pid)
        } catch {
            Add-Failure "Could not reap own crash reporter PID $($reporter.pid): $($_.Exception.Message)"
        }
    }
    return @($reaped)
}

function Get-NewCrashes($Before, [datetime]$StartedUtc) {
    $after = Get-CrashSnapshot
    $new = [System.Collections.Generic.List[string]]::new()
    foreach ($path in $after.Keys) {
        if (-not $Before.Contains($path) -or [int64]$after[$path] -ge $StartedUtc.ToUniversalTime().Ticks) {
            [void]$new.Add($path)
        }
    }
    return @($new)
}

function Wait-OwnedProcess($Process, [int]$TimeoutSeconds) {
    $finished = $Process.WaitForExit($TimeoutSeconds * 1000)
    if ($finished) {
        [void]$Process.WaitForExit()
        return [ordered]@{ timed_out = $false; exit_code = [int]$Process.ExitCode }
    }
    # This PID came from our Start-Process call. Never use a name-based kill or a broad process
    # query here: an owner's editor may be unrelated and must remain untouched.
    try { Stop-Process -Id ([int]$Process.Id) -Force -ErrorAction Stop } catch { }
    try { [void]$Process.WaitForExit(5000) } catch { }
    return [ordered]@{ timed_out = $true; exit_code = $null }
}

function Invoke-UnrealLaunch([string]$Kind, [string[]]$Arguments, [string]$LogPath,
                             [string]$StdoutPath, [string]$StderrPath) {
    Assert-NoForeignUnreal "before $Kind launch"
    Ensure-Directory (Split-Path -Parent $LogPath)
    Ensure-Directory (Split-Path -Parent $StdoutPath)
    Ensure-Directory (Split-Path -Parent $StderrPath)
    $startedUtc = [datetime]::UtcNow
    $crashesBefore = Get-CrashSnapshot
    $reportersBefore = @(Get-CrashReporterProcesses)
    $ledger = [ordered]@{
        kind = $Kind
        pid = $null
        started_utc = $startedUtc.ToString('o')
        ended_utc = $null
        exit_code = $null
        timed_out = $false
        crash_reporters_reaped = @()
        new_crashes = @()
        log = $LogPath
        stdout = $StdoutPath
        stderr = $StderrPath
        arguments = @($Arguments)
        status = 'not_started'
    }
    $isolatedLocalAppData = Join-Path $RunRoot 'LocalAppData'
    Ensure-Directory $isolatedLocalAppData
    $oldLocalAppData = $env:LOCALAPPDATA
    try {
        # Unreal resolves several engine-wide stores from LOCALAPPDATA even when -userdir and
        # -LocalDataCachePath are supplied. Keep this child process inside the run directory.
        $env:LOCALAPPDATA = $isolatedLocalAppData
        $process = Start-Process -FilePath $EditorExe -ArgumentList $Arguments -WorkingDirectory $HostRoot `
            -RedirectStandardOutput $StdoutPath -RedirectStandardError $StderrPath -WindowStyle Hidden -PassThru
        $ledger.pid = [int]$process.Id
        $wait = Wait-OwnedProcess $process $LaunchTimeoutSeconds
        $ledger.timed_out = [bool]$wait.timed_out
        $ledger.exit_code = $wait.exit_code
        $ledger.status = if ($ledger.timed_out) { 'timeout' } elseif ($ledger.exit_code -eq 0) { 'passed' } else { 'failed' }
    } catch {
        $ledger.status = 'launch_error'
        $ledger.error = $_.Exception.Message
    } finally {
        if ($null -eq $oldLocalAppData) { Remove-Item Env:LOCALAPPDATA -ErrorAction SilentlyContinue } else { $env:LOCALAPPDATA = $oldLocalAppData }
    }
    $ledger.isolated_local_app_data = $isolatedLocalAppData
    $ledger.ended_utc = [datetime]::UtcNow.ToString('o')
    $ledger.crash_reporters_reaped = @(Reap-OwnCrashReporters $reportersBefore $startedUtc $LogPath)
    $ledger.new_crashes = @(Get-NewCrashes $crashesBefore $startedUtc)
    [void]$LaunchLedger.Add($ledger)
    if ($ledger.status -ne 'passed') {
        Add-Failure "$Kind launch $($ledger.status) (PID=$($ledger.pid), exit=$($ledger.exit_code))"
    }
    if ($ledger.new_crashes.Count -gt 0) {
        Add-Failure "$Kind produced new UECC crash directory: $($ledger.new_crashes[0])"
    }
    return $ledger
}

function Invoke-BuildProcess([string[]]$Arguments, [string]$LogPath, [string]$ErrorPath,
                             [string]$StdoutPath = $LogPath) {
    Ensure-Directory (Split-Path -Parent $LogPath)
    $dotnetHome = Join-Path $RunRoot 'DotnetHome'
    Ensure-Directory $dotnetHome
    $dotnet = (Get-Command dotnet.exe -ErrorAction Stop).Source
    $started = [datetime]::UtcNow
    $oldDotnetCliHome = $env:DOTNET_CLI_HOME
    $oldDotnetFirstUse = $env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE
    $oldDotnetNoLogo = $env:DOTNET_NOLOGO
    $isolatedLocalAppData = Join-Path $RunRoot 'LocalAppData'
    Ensure-Directory $isolatedLocalAppData
    $oldLocalAppData = $env:LOCALAPPDATA
    try {
        # Keep the build tool's first-use sentinel and package metadata inside this run. This is
        # separate from Unreal's LocalAppData guard and avoids touching the owner's profile.
        $env:DOTNET_CLI_HOME = $dotnetHome
        $env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
        $env:DOTNET_NOLOGO = '1'
        $env:LOCALAPPDATA = $isolatedLocalAppData
        $process = Start-Process -FilePath $dotnet -ArgumentList $Arguments -WorkingDirectory $PluginRoot `
            -RedirectStandardOutput $StdoutPath -RedirectStandardError $ErrorPath -WindowStyle Hidden -PassThru
        $wait = Wait-OwnedProcess $process $LaunchTimeoutSeconds
    } finally {
        if ($null -eq $oldDotnetCliHome) { Remove-Item Env:DOTNET_CLI_HOME -ErrorAction SilentlyContinue } else { $env:DOTNET_CLI_HOME = $oldDotnetCliHome }
        if ($null -eq $oldDotnetFirstUse) { Remove-Item Env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE -ErrorAction SilentlyContinue } else { $env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = $oldDotnetFirstUse }
        if ($null -eq $oldDotnetNoLogo) { Remove-Item Env:DOTNET_NOLOGO -ErrorAction SilentlyContinue } else { $env:DOTNET_NOLOGO = $oldDotnetNoLogo }
        if ($null -eq $oldLocalAppData) { Remove-Item Env:LOCALAPPDATA -ErrorAction SilentlyContinue } else { $env:LOCALAPPDATA = $oldLocalAppData }
    }
    return [ordered]@{
        tool = $dotnet
        pid = [int]$process.Id
        started_utc = $started.ToString('o')
        ended_utc = [datetime]::UtcNow.ToString('o')
        exit_code = $wait.exit_code
        timed_out = [bool]$wait.timed_out
        log = $LogPath
        stdout = $StdoutPath
        error_log = $ErrorPath
        arguments = @($Arguments)
        isolated_local_app_data = $isolatedLocalAppData
        status = if ($wait.timed_out) { 'timeout' } elseif ($wait.exit_code -eq 0) { 'passed' } else { 'failed' }
    }
}

function Copy-DirectoryContents([string]$Source, [string]$Destination) {
    Ensure-Directory $Destination
    foreach ($item in @(Get-ChildItem -LiteralPath $Source -Force)) {
        $target = Join-Path $Destination $item.Name
        $null = Copy-Item -LiteralPath $item.FullName -Destination $target -Recurse -Force -PassThru
    }
}

function Sync-StagedHost {
    if (-not (Test-Path -LiteralPath $HostProject -PathType Leaf)) {
        throw "Staged host project is missing: $HostProject"
    }
    if (-not (Test-Path -LiteralPath $StagePlugin -PathType Container)) {
        throw "Staged VoxelForge plugin is missing: $StagePlugin"
    }
    $stageSource = Join-Path $StagePlugin 'Source'
    if (Test-Path -LiteralPath $stageSource -PathType Container) {
        if (-not (Test-UnderPath $stageSource $StagePlugin)) {
            throw "Refusing to clean a staged source path outside the staged plugin: $stageSource"
        }
        Remove-Item -LiteralPath $stageSource -Recurse -Force
    }
    Copy-DirectoryContents (Join-Path $PluginRoot 'Source') $stageSource
    $null = Copy-Item -LiteralPath (Join-Path $PluginRoot 'VoxelForge.uplugin') `
        -Destination (Join-Path $StagePlugin 'VoxelForge.uplugin') -Force -PassThru

    # The staged map is a host copy; these copies never write the owner's project.
    $stageContent = Join-Path $HostRoot 'Content'
    $stageVoxelContent = Join-Path $stageContent 'VoxelForge'
    Ensure-Directory $stageVoxelContent
    foreach ($relative in @('Untitled.umap', 'VoxelForge\BP_VoxelWorld.uasset')) {
        $source = Join-Path (Join-Path $ProjectRoot 'Content') $relative
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            $target = Join-Path $stageContent $relative
            Ensure-Directory (Split-Path -Parent $target)
            $null = Copy-Item -LiteralPath $source -Destination $target -Force -PassThru
        }
    }
}

function Sync-OwnerAssets {
    $rows = [System.Collections.Generic.List[object]]::new()
    # Composer tests discover the authored strate corpus from the staged asset registry. Keep the
    # complete owner corpus here; the explore commandlet still selects DA_Strate3 explicitly for
    # the owner-world scenario below.
    foreach ($assetName in @('DA_Strate1', 'DA_Strate2', 'DA_Strate3', 'DA_Strate4', 'DA_Settings')) {
        $source = Join-Path $ProjectRoot "Content\VoxelForge\$assetName.uasset"
        $target = Join-Path $HostRoot "Content\VoxelForge\$assetName.uasset"
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Owner asset is missing: $source"
        }
        Ensure-Directory (Split-Path -Parent $target)
        $null = Copy-Item -LiteralPath $source -Destination $target -Force -PassThru
        [void]$rows.Add([ordered]@{
            name = $assetName
            used_by_scenario = $Scenario -in @('owner', 'probe', 'parity', 'tests', 'surface-fall', 'crossing', 'gate-stress')
            source_path = $source
            source_sha256 = Get-Sha256 $source
            staged_path = $target
            staged_sha256 = Get-Sha256 $target
        })
    }
    return @($rows)
}

function Sync-TestCharacterFixture {
    $rows = [System.Collections.Generic.List[object]]::new()

    # ScaleDiagnosis reads the owner's character CDO for movement limits.  The isolated host
    # already contains the CDO, but the CDO's animation blueprint loads the complete unarmed
    # blend-space sample set.  Keep that dependency closure in the harness instead of turning a
    # missing staged asset into an automation-test error.  These are copies into the host only.
    $relativeDirectories = @('Characters\Mannequins\Anims\Unarmed')
    foreach ($relativeDirectory in $relativeDirectories) {
        $sourceDirectory = Join-Path $ProjectRoot "Content\$relativeDirectory"
        $targetDirectory = Join-Path $HostRoot "Content\$relativeDirectory"
        if (-not (Test-Path -LiteralPath $sourceDirectory -PathType Container)) {
            throw "Test fixture directory is missing: $sourceDirectory"
        }
        Copy-DirectoryContents $sourceDirectory $targetDirectory
        foreach ($source in @(Get-ChildItem -LiteralPath $sourceDirectory -Recurse -File)) {
            $relativeFile = $source.FullName.Substring($sourceDirectory.Length).TrimStart('\')
            $target = Join-Path $targetDirectory $relativeFile
            [void]$rows.Add([ordered]@{
                name = "TestFixture/$relativeDirectory/$relativeFile"
                used_by_scenario = $true
                source_path = $source.FullName
                source_sha256 = Get-Sha256 $source.FullName
                staged_path = $target
                staged_sha256 = Get-Sha256 $target
            })
        }
    }
    return @($rows)
}

function Get-AssetEvidence {
    $rows = [System.Collections.Generic.List[object]]::new()
    foreach ($assetName in @('DA_Strate1', 'DA_Strate2', 'DA_Strate3', 'DA_Strate4', 'DA_Settings')) {
        $source = Join-Path $ProjectRoot "Content\VoxelForge\$assetName.uasset"
        $target = Join-Path $HostRoot "Content\VoxelForge\$assetName.uasset"
        [void]$rows.Add([ordered]@{
            name = $assetName
            used_by_scenario = $Scenario -in @('owner', 'probe', 'parity', 'tests', 'surface-fall', 'crossing', 'gate-stress')
            source_path = $source
            source_sha256 = Get-Sha256 $source
            staged_path = $target
            staged_sha256 = Get-Sha256 $target
        })
    }
    return @($rows)
}

function Get-ValueText($Value) {
    if ($Value -is [bool]) { return $(if ($Value) { '1' } else { '0' }) }
    if ($Value -is [System.Array]) { return (($Value | ForEach-Object { Get-ValueText $_ }) -join ',') }
    return [string]$Value
}

function Merge-Cvars([System.Collections.IDictionary]$Base) {
    $merged = [ordered]@{}
    foreach ($key in $Base.Keys) { $merged[[string]$key] = $Base[$key] }
    if ($null -ne $Cvars) {
        foreach ($key in $Cvars.Keys) { $merged[[string]$key] = $Cvars[$key] }
    }
    return $merged
}

function New-CommonArguments([string]$Kind, [string]$LogPath, $CvarMap) {
    $safeLabel = ($Label -replace '[^A-Za-z0-9_.-]', '_')
    $args = [System.Collections.Generic.List[string]]::new()
    # Quoting the whole -project= token is intentional: this is the failure mode that used to
    # swallow every argument after "Projet Unreal".
    [void]$args.Add(('-project="{0}"' -f $HostProject))
    [void]$args.Add('-nullrhi')
    [void]$args.Add('-DDC-ForceMemoryCache')
    [void]$args.Add('-unattended')
    [void]$args.Add('-nop4')
    [void]$args.Add('-nosplash')
    [void]$args.Add('-NoSound')
    [void]$args.Add('-stdout')
    [void]$args.Add('-FullStdOutLogOutput')
    [void]$args.Add('-forcelogflush')
    [void]$args.Add('-NoZenAutoLaunch')
    [void]$args.Add('-nocrashreports')
    [void]$args.Add('-WindowsPlatformCrashContext.ForceCrashReportDialogOff=1')
    [void]$args.Add("-enginesaveddirsuffix=VoxelForgeTest_$safeLabel")
    [void]$args.Add(('-userdir="{0}"' -f (Join-Path $RunRoot 'User')))
    [void]$args.Add(('-ZenDataPath="{0}"' -f (Join-Path $RunRoot 'ZenData')))
    [void]$args.Add('-ddc=NoZenLocalFallback')
    [void]$args.Add(('-LocalDataCachePath="{0}"' -f (Join-Path $RunRoot 'DDC')))
    [void]$args.Add(('-abslog="{0}"' -f $LogPath))
    if ($Kind -eq 'commandlet') {
        [void]$args.Add('-run=VoxelForgeExplore')
    } elseif ($Kind -eq 'game') {
        [void]$args.Add('-game')
        [void]$args.Add('-map=/Game/Untitled')
    }
    foreach ($key in @($CvarMap.Keys | Sort-Object)) {
        $switch = [string]$key
        if (-not $switch.StartsWith('-')) { $switch = "-$switch" }
        $value = Get-ValueText $CvarMap[$key]
        if ($value -match '\s') {
            [void]$args.Add(('{0}="{1}"' -f $switch, $value))
        } else {
            [void]$args.Add("$switch=$value")
        }
    }
    return @($args)
}

function New-CommandletArguments([string]$CommandletOut, [string]$Modes, [bool]$UseOwnerAssets,
                                 [bool]$GameRuntimeConfig, [int]$ExportSize = 128,
                                 [int]$Slot = 1) {
    $args = [System.Collections.Generic.List[string]]::new()
    [void]$args.Add('-seed=0')
    [void]$args.Add('-archetype=TunnelNetwork')
    [void]$args.Add("-slot=$Slot")
    [void]$args.Add('-opstack=1')
    [void]$args.Add(('-modes="{0}"' -f $Modes))
    [void]$args.Add('-blockearlyout=0')
    [void]$args.Add('-densitygridreuse=1')
    [void]$args.Add("-exportsize=$ExportSize")
    if ($GameRuntimeConfig) { [void]$args.Add('-gameconfig=1') }
    if ($UseOwnerAssets) {
        [void]$args.Add('-strateref=/Game/VoxelForge/DA_Strate3.DA_Strate3')
        [void]$args.Add('-settingsref=/Game/VoxelForge/DA_Settings.DA_Settings')
    }
    [void]$args.Add(('-out="{0}"' -f $CommandletOut))
    return @($args)
}

function Read-JsonFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json)
}

function Get-CommandletMetrics([string]$OutputDirectory, [string]$Use) {
    $jsonPath = Join-Path $OutputDirectory 'explore.json'
    $json = Read-JsonFile $jsonPath
    if ($null -eq $json) {
        return [ordered]@{ status = 'missing'; output = $OutputDirectory }
    }
    $summary = Get-JsonValue $json 'summary'
    $export = Get-JsonValue $json 'export'
    $walk = Get-JsonValue $json 'walk'
    $ledger = Get-JsonValue $summary 'tile_ledger'
    $objPath = Join-Path $OutputDirectory 'geometry.obj'
    $capabilities = [ordered]@{}
    foreach ($name in @('player_fit_volume_cells', 'reachable_player_fit_cells',
                        'walk_only_reachable_player_fit_cells', 'climb_reachable_player_fit_cells',
                        'arrival_passage_count', 'departure_passage_count', 'triangle_count',
                        'tile_count', 'completed_tiles')) {
        $value = Get-JsonValue $walk $name $null
        if ($null -eq $value) { $value = Get-JsonValue $summary $name $null }
        if ($null -eq $value) { $value = Get-JsonValue $export $name $null }
        if ($null -ne $value) { $capabilities[$name] = $value }
    }
    $probe = Get-JsonValue $json 'tunnel_core_probe'
    $probeAmplitudes = [ordered]@{}
    foreach ($sectionName in @('floor', 'wall', 'ceiling', 'mouth_probe')) {
        $section = Get-JsonValue $probe $sectionName $null
        if ($null -eq $section) { continue }
        foreach ($property in @($section.PSObject.Properties)) {
            if ($property.Name -match 'amplitude|period') {
                $probeAmplitudes["$sectionName.$($property.Name)"] = $property.Value
            }
        }
    }
    foreach ($name in @('periodic_amplitude_voxels', 'periodic_amplitude_m',
                        'amplitude_voxels', 'amplitude', 'periodic_period_voxels')) {
        $value = Get-JsonValue $probe $name $null
        if ($null -ne $value) { $probeAmplitudes[$name] = $value }
    }
    $meshSeconds = Get-JsonValue $summary 'mesh_seconds' $null
    $workerSeconds = Get-JsonValue (Get-JsonValue $summary 'mesher_tasks') 'job_sum_seconds' $meshSeconds
    $connectivity = [ordered]@{
        result = Get-JsonValue $walk 'connectivity_result' $null
        can_reach_departure = Get-JsonValue $walk 'can_reach_departure' $null
        walk_only_can_reach_departure = Get-JsonValue $walk 'walk_only_can_reach_departure' $null
    }
    return [ordered]@{
        status = Get-JsonValue $json 'status' 'ok'
        use = $Use
        output = $OutputDirectory
        json = $jsonPath
        capability_counts = $capabilities
        connectivity = $connectivity
        geometry_hash = Get-JsonValue $summary 'geometry_hash' (Get-JsonValue $export 'geometry_hash' $null)
        obj_sha256 = Get-Sha256 $objPath
        obj_path = $objPath
        probe_amplitudes = $probeAmplitudes
        worker_seconds = $workerSeconds
        mesh_seconds = $meshSeconds
        tile_ledger = $ledger
        input = Get-JsonValue $json 'input' $null
        game_runtime_config = Get-JsonValue (Get-JsonValue $json 'input') 'game_runtime_config' $false
    }
}

function Get-StreamingMetrics([string]$LogPath) {
    $lineMatch = @(Select-String -LiteralPath $LogPath -Pattern '\[VoxelForgeStreamingLatency\]' -SimpleMatch:$false -ErrorAction SilentlyContinue |
        Select-Object -Last 1)
    if ($lineMatch.Count -eq 0) { return [ordered]@{ status = 'missing'; log = $LogPath } }
    $line = [string]$lineMatch[0].Line
    $metrics = [ordered]@{ status = 'ok'; log = $LogPath; raw = $line }
    $patterns = [ordered]@{
        lod0_ready_samples = 'lod0_ready_samples=(?<v>[-+0-9.eE]+)'
        request_to_ready_p50 = 'request_to_ready_s\[p50=(?<v>[-+0-9.eE]+)'
        request_to_ready_p95 = 'request_to_ready_s\[p50=[^]]*p95=(?<v>[-+0-9.eE]+)'
        queue_wait_p50 = 'queue_wait_s\[p50=(?<v>[-+0-9.eE]+)'
        queue_wait_p95 = 'queue_wait_s\[p50=[^]]*p95=(?<v>[-+0-9.eE]+)'
        generation_p50 = 'generation_s\[p50=(?<v>[-+0-9.eE]+)'
        generation_p95 = 'generation_s\[p50=[^]]*p95=(?<v>[-+0-9.eE]+)'
        worker_generation_sum_s = 'worker_generation_sum_s=(?<v>[-+0-9.eE]+)'
        applied_tiles = 'applied_tiles=(?<v>[-+0-9.eE]+)'
        applied_triangles = 'applied_triangles=(?<v>[-+0-9.eE]+)'
    }
    foreach ($key in $patterns.Keys) {
        $match = [regex]::Match($line, $patterns[$key])
        if ($match.Success) { $metrics[$key] = $match.Groups['v'].Value }
    }
    $metrics['p50_p95'] = [ordered]@{
        request_to_ready = [ordered]@{ p50 = $metrics['request_to_ready_p50']; p95 = $metrics['request_to_ready_p95'] }
        queue_wait = [ordered]@{ p50 = $metrics['queue_wait_p50']; p95 = $metrics['queue_wait_p95'] }
        generation = [ordered]@{ p50 = $metrics['generation_p50']; p95 = $metrics['generation_p95'] }
    }
    return $metrics
}

function Get-SurfaceFallMetrics([string]$LogPath) {
    $lineMatch = @(Select-String -LiteralPath $LogPath -Pattern '\[VoxelForgeSurfaceFallTest\] result=' -ErrorAction SilentlyContinue |
        Select-Object -Last 1)
    if ($lineMatch.Count -eq 0) { return [ordered]@{ status = 'missing'; log = $LogPath } }
    $line = [string]$lineMatch[0].Line
    $metrics = [ordered]@{ status = 'failed'; log = $LogPath; raw = $line }
    $result = [regex]::Match($line, 'result=(?<v>PASS|FAIL)')
    if ($result.Success -and $result.Groups['v'].Value -eq 'PASS') { $metrics.status = 'passed' }
    foreach ($key in @('reason', 'elapsed_s', 'distance_m', 'terrain_vox')) {
        $match = [regex]::Match($line, ('{0}=(?<v>[^ ]+)' -f $key))
        if ($match.Success) { $metrics[$key] = $match.Groups['v'].Value }
    }
    return $metrics
}

function Get-StrateCrossingMetrics([string]$LogPath) {
    $lineMatch = @(Select-String -LiteralPath $LogPath -Pattern '\[VoxelForgeStrateCrossingTest\] result=' -ErrorAction SilentlyContinue |
        Select-Object -Last 1)
    if ($lineMatch.Count -eq 0) { return [ordered]@{ status = 'missing'; log = $LogPath } }
    $line = [string]$lineMatch[0].Line
    $metrics = [ordered]@{ status = 'failed'; log = $LogPath; raw = $line }
    $result = [regex]::Match($line, 'result=(?<v>PASS|FAIL)')
    if ($result.Success -and $result.Groups['v'].Value -eq 'PASS') { $metrics.status = 'passed' }
    foreach ($key in @('reason', 'elapsed_s', 'distance_m', 'max_gate_s', 'gate_limit_s', 'gate_total_s', 'min_floor_clearance_vox')) {
        $match = [regex]::Match($line, ('{0}=(?<v>[^ ]+)' -f $key))
        if ($match.Success) { $metrics[$key] = $match.Groups['v'].Value }
    }
    return $metrics
}

function Get-CollisionGateStressMetrics([string]$LogPath) {
    $lineMatch = @(Select-String -LiteralPath $LogPath -Pattern '\[VoxelForgeCollisionGateStressTest\] result=' -ErrorAction SilentlyContinue |
        Select-Object -Last 1)
    if ($lineMatch.Count -eq 0) { return [ordered]@{ status = 'missing'; log = $LogPath } }
    $line = [string]$lineMatch[0].Line
    $metrics = [ordered]@{ status = 'failed'; log = $LogPath; raw = $line }
    $result = [regex]::Match($line, 'result=(?<v>PASS|FAIL)')
    if ($result.Success -and $result.Groups['v'].Value -eq 'PASS') { $metrics.status = 'passed' }
    foreach ($key in @('reason', 'elapsed_s', 'distance_m', 'expected_distance_m', 'speed_cm_s',
                        'gate_holds', 'gate_total_s', 'max_gate_s', 'full_stops',
                        'full_stop_total_s', 'collision_gate')) {
        $match = [regex]::Match($line, ('{0}=(?<v>[^ ]+)' -f $key))
        if ($match.Success) { $metrics[$key] = $match.Groups['v'].Value }
    }
    return $metrics
}

function Get-TestMetrics($Ledger) {
    if ($null -eq $Ledger) { return @() }
    $logPaths = @($Ledger.log, $Ledger.stdout, $Ledger.stderr) | Where-Object {
        $_ -and (Test-Path -LiteralPath $_ -PathType Leaf)
    }
    $rows = [System.Collections.Generic.List[object]]::new()
    $seen = @{}
    foreach ($path in $logPaths) {
        foreach ($matchLine in @(Select-String -LiteralPath $path -Pattern 'Test Completed\. Result=' -ErrorAction SilentlyContinue)) {
            $match = [regex]::Match([string]$matchLine.Line,
                'Test Completed\. Result=\{(?<result>[^}]+)\} Name=\{(?<name>[^}]+)\} Path=\{(?<path>[^}]+)\}')
            if (-not $match.Success) { continue }
            $testPath = $match.Groups['path'].Value
            $key = if ([string]::IsNullOrWhiteSpace($testPath)) { $match.Groups['name'].Value } else { $testPath }
            if ($seen.ContainsKey($key)) { continue }
            $seen[$key] = $true
            $rawResult = $match.Groups['result'].Value
            $status = if ($rawResult -match '^(Success|Passed|Pass)$') { 'passed' } else { 'failed' }
            [void]$rows.Add([ordered]@{
                name = $match.Groups['name'].Value
                path = $testPath
                status = $status
                raw_result = $rawResult
            })
        }
    }
    return @($rows | Sort-Object path, name)
}

function Get-ParityKey($Tile) {
    return "$($Tile.absolute_tile_x):$($Tile.absolute_tile_y):$($Tile.absolute_tile_z):$($Tile.lod)"
}

function Get-ParityConfigEvidence([string]$CommandletLogPath, [string]$GameLogPath) {
    $pattern = '\[StrateManager\] (Strate |Initialized|Operator-stack)|\[VoxelForgeExplore\]\[OwnerAssets\]'
    $commandletLines = @()
    $gameLines = @()
    if (Test-Path -LiteralPath $CommandletLogPath -PathType Leaf) {
        $commandletLines = @(Select-String -LiteralPath $CommandletLogPath -Pattern $pattern -ErrorAction SilentlyContinue |
            ForEach-Object { $_.Line })
    }
    if (Test-Path -LiteralPath $GameLogPath -PathType Leaf) {
        $gameLines = @(Select-String -LiteralPath $GameLogPath -Pattern $pattern -ErrorAction SilentlyContinue |
            ForEach-Object { $_.Line })
    }
    $getCount = {
        param([object[]]$Lines)
        foreach ($line in $Lines) {
            $match = [regex]::Match([string]$line, 'Initialized (?<count>\d+) strates')
            if ($match.Success) { return [int]$match.Groups['count'].Value }
        }
        return $null
    }
    $commandletCount = & $getCount $commandletLines
    $gameCount = & $getCount $gameLines
    return [ordered]@{
        commandlet = [ordered]@{
            initialized_strates = $commandletCount
            relevant_log_lines = @($commandletLines | Select-Object -Last 30)
        }
        game = [ordered]@{
            initialized_strates = $gameCount
            relevant_log_lines = @($gameLines | Select-Object -Last 30)
        }
        layout_matches = ($null -ne $commandletCount -and $null -ne $gameCount -and $commandletCount -eq $gameCount)
        interpretation = 'StrateManager lines expose the authored settings/strate layout used by each path; tile step/cells and content-band fields remain in the per-tile records.'
    }
}

function Compare-Parity($CommandletJson, [string]$GameDumpPath,
                        [string]$CommandletLogPath, [string]$GameLogPath) {
    $expected = @((Get-JsonValue (Get-JsonValue $CommandletJson 'summary') 'tile_ledger').tiles)
    $gameJson = Read-JsonFile $GameDumpPath
    $configEvidence = Get-ParityConfigEvidence $CommandletLogPath $GameLogPath
    if ($null -eq $gameJson) {
        return [ordered]@{
            status = 'failed'
            reason = "game tile hash dump missing: $GameDumpPath"
            configuration = $configEvidence
        }
    }
    $gameByKey = @{}
    foreach ($tile in @((Get-JsonValue $gameJson 'records'))) {
        $key = "$($tile.tile_x):$($tile.tile_y):$($tile.tile_z):$($tile.level)"
        $gameByKey[$key] = $tile
    }
    $firstDifference = $null
    foreach ($tile in $expected) {
        $key = Get-ParityKey $tile
        if (-not $gameByKey.ContainsKey($key)) {
            $firstDifference = [ordered]@{ tile = $key; reason = 'missing_from_game'; commandlet = $tile }
            break
        }
        $gameTile = $gameByKey[$key]
        if ([string]$tile.geometry_hash -ne [string]$gameTile.geometry_hash) {
            $diagnosis = [System.Collections.Generic.List[string]]::new()
            if ([int]$gameTile.step -ne 1 -or [int]$gameTile.cells -ne 32) {
                [void]$diagnosis.Add('LOD/step/cells differ from the level-0 export')
            }
            if ([int]$gameTile.band_chunk_lo -ne -2147483648 -or [int]$gameTile.band_chunk_hi -ne 2147483647) {
                [void]$diagnosis.Add('runtime content-band cut is active')
            }
            if ($configEvidence.layout_matches -eq $false) {
                [void]$diagnosis.Add(('settings/strate layout differs: commandlet initialized {0}, game initialized {1}' -f
                    $configEvidence.commandlet.initialized_strates, $configEvidence.game.initialized_strates))
            }
            [void]$diagnosis.Add('cvar defaults and authored settings are recorded in both artifacts')
            [void]$diagnosis.Add('if those inputs match, the remaining candidate is the fused evaluator/runtime route')
            $firstDifference = [ordered]@{
                tile = $key
                reason = 'geometry_hash_mismatch'
                commandlet_hash = [string]$tile.geometry_hash
                game_hash = [string]$gameTile.geometry_hash
                commandlet_tile = $tile
                game_tile = $gameTile
                diagnosis = @($diagnosis)
            }
            break
        }
    }
    if ($null -ne $firstDifference) {
        return [ordered]@{
            status = 'failed'
            first_difference = $firstDifference
            commandlet_tile_count = $expected.Count
            game_record_count = @((Get-JsonValue $gameJson 'records')).Count
            game_dump = $GameDumpPath
            configuration = $configEvidence
        }
    }
    return [ordered]@{
        status = 'passed'
        compared_tile_count = $expected.Count
        commandlet_tile_count = $expected.Count
        game_record_count = @((Get-JsonValue $gameJson 'records')).Count
        game_dump = $GameDumpPath
        configuration = $configEvidence
    }
}

try {
    if ([string]::IsNullOrWhiteSpace($Label)) { $Label = (Get-Date).ToUniversalTime().ToString('yyyyMMdd_HHmmss') }
    $Label = ($Label -replace '[^A-Za-z0-9_.-]', '_')
    if ([string]::IsNullOrWhiteSpace($Out)) {
        $Out = Join-Path $PluginRoot "Saved\TestHarness\${Label}_${Scenario}"
    }
    elseif (-not [IO.Path]::IsPathRooted($Out)) {
        throw "-Out must be an absolute path: $Out"
    }
    $RunRoot = Get-FullPath $Out
    if (-not (Test-UnderPath $RunRoot (Join-Path $PluginRoot 'Saved'))) {
        throw "-Out must stay under the plugin Saved directory: $RunRoot"
    }
    Ensure-Directory $RunRoot
    $ResultPath = Join-Path $RunRoot 'result.json'
    $SummaryPath = Join-Path $RunRoot 'summary.txt'
    Ensure-Directory (Join-Path $RunRoot 'Logs')
    Ensure-Directory (Join-Path $RunRoot 'User')
    Ensure-Directory (Join-Path $RunRoot 'ZenData')
    Ensure-Directory (Join-Path $RunRoot 'DDC')
    $localBefore = Get-StateSnapshot $LocalAppDataRoot

    foreach ($required in @($EditorExe, $UbtDll, $HostProject, $ProjectFile)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required file is missing: $required" }
    }
    if ($Build) {
        $sourceBefore = Get-SourceSnapshot $PluginRoot
        $stageBefore = Get-SourceSnapshot $StagePlugin
        $sourceChangedKeys = @(Get-ChangedSnapshotKeys $stageBefore $sourceBefore)
        $runtimeBefore = Get-Sha256 (Join-Path $StagePlugin 'Binaries\Win64\UnrealEditor-VoxelForge.dll')
        $editorBefore = Get-Sha256 (Join-Path $StagePlugin 'Binaries\Win64\UnrealEditor-VoxelForgeEditor.dll')
        Assert-NoForeignUnreal 'before staged sync/build'
        Sync-StagedHost
        $buildLog = Join-Path $RunRoot 'Logs\ubt.log'
        $buildStdout = Join-Path $RunRoot 'Logs\build.stdout.log'
        $buildError = Join-Path $RunRoot 'Logs\build.err.log'
        $buildArgs = @(('"{0}"' -f $UbtDll), 'UnrealEditor', 'Win64', 'Development', ('-Project="{0}"' -f $HostProject),
            '-WaitMutex', '-FromMsBuild', '-architecture=x64', '-NoUBA', '-MaxParallelActions=4',
            ('-Log="{0}"' -f $buildLog))
        $BuildRecord = Invoke-BuildProcess $buildArgs $buildLog $buildError $buildStdout
        if ($BuildRecord.status -ne 'passed') { Add-Failure "staged build $($BuildRecord.status)" }
        $runtimeAfter = Get-Sha256 (Join-Path $StagePlugin 'Binaries\Win64\UnrealEditor-VoxelForge.dll')
        $editorAfter = Get-Sha256 (Join-Path $StagePlugin 'Binaries\Win64\UnrealEditor-VoxelForgeEditor.dll')
        $BuildRecord.source_changed_keys = @($sourceChangedKeys)
        $BuildRecord.runtime_dll_before = $runtimeBefore
        $BuildRecord.runtime_dll_after = $runtimeAfter
        $BuildRecord.editor_dll_before = $editorBefore
        $BuildRecord.editor_dll_after = $editorAfter
        $runtimeChanged = @($sourceChangedKeys | Where-Object { $_ -like 'source\voxelforge\*' }).Count -gt 0
        $editorChanged = @($sourceChangedKeys | Where-Object { $_ -like 'source\voxelforgeeditor\*' }).Count -gt 0
        if ($runtimeChanged -and $runtimeBefore -eq $runtimeAfter) {
            throw 'Source changed under Source/VoxelForge but the staged runtime DLL SHA-256 did not change.'
        }
        if ($editorChanged -and $editorBefore -eq $editorAfter) {
            throw 'Source changed under Source/VoxelForgeEditor but the staged editor DLL SHA-256 did not change.'
        }
    }
    if ($Assets -eq 'owner') {
        $AssetEvidence = @(Sync-OwnerAssets)
    } else {
        $AssetEvidence = @(Get-AssetEvidence)
    }
    if ($Scenario -eq 'tests') {
        $AssetEvidence += @(Sync-TestCharacterFixture)
    }
    $DllEvidence.runtime = [ordered]@{
        path = Join-Path $StagePlugin 'Binaries\Win64\UnrealEditor-VoxelForge.dll'
        sha256 = Get-Sha256 (Join-Path $StagePlugin 'Binaries\Win64\UnrealEditor-VoxelForge.dll')
    }
    $DllEvidence.editor = [ordered]@{
        path = Join-Path $StagePlugin 'Binaries\Win64\UnrealEditor-VoxelForgeEditor.dll'
        sha256 = Get-Sha256 (Join-Path $StagePlugin 'Binaries\Win64\UnrealEditor-VoxelForgeEditor.dll')
    }
    if (-not $DllEvidence.runtime.sha256 -or -not $DllEvidence.editor.sha256) {
        throw "Staged runtime/editor DLL is missing under $StagePlugin; use -Build."
    }

    # VoxelForgeExplore deliberately rejects output outside its staged plugin Saved root. The
    # harness result stays in $RunRoot; this is the commandlet's internal artifact directory.
    $commandletOut = Join-Path $StagePlugin "Saved\TestHarness\${Label}_${Scenario}"
    $baseCvars = [ordered]@{}
    $commandletLedger = $null
    $gameLedger = $null
    switch ($Scenario) {
        'canonical' {
            $baseCvars['voxel.OuterClassifierMode'] = 0
            $args = New-CommonArguments 'commandlet' (Join-Path $RunRoot 'Logs\canonical.log') (Merge-Cvars $baseCvars)
            $args += New-CommandletArguments $commandletOut 'walk,export,tunnelcoreprobe' $false $false 128
            $commandletLedger = Invoke-UnrealLaunch 'canonical-commandlet' $args `
                (Join-Path $RunRoot 'Logs\canonical.log') (Join-Path $RunRoot 'Logs\canonical.stdout.log') (Join-Path $RunRoot 'Logs\canonical.stderr.log')
            $CommandletMetrics = Get-CommandletMetrics $commandletOut 'synthetic canonical'
            $canonicalObjSha = Get-JsonValue $CommandletMetrics 'obj_sha256'
            if ($canonicalObjSha -and $canonicalObjSha -ne $CanonicalObjSha256) {
                Add-Failure "canonical OBJ SHA-256 changed: expected $CanonicalObjSha256, got $canonicalObjSha"
            }
        }
        'owner' {
            $args = New-CommonArguments 'commandlet' (Join-Path $RunRoot 'Logs\owner.log') (Merge-Cvars $baseCvars)
            $args += New-CommandletArguments $commandletOut 'walk,export' ($Assets -eq 'owner') $false 128
            $commandletLedger = Invoke-UnrealLaunch 'owner-commandlet' $args `
                (Join-Path $RunRoot 'Logs\owner.log') (Join-Path $RunRoot 'Logs\owner.stdout.log') (Join-Path $RunRoot 'Logs\owner.stderr.log')
            $CommandletMetrics = Get-CommandletMetrics $commandletOut 'owner assets'
        }
        'probe' {
            $args = New-CommonArguments 'commandlet' (Join-Path $RunRoot 'Logs\probe.log') (Merge-Cvars $baseCvars)
            $args += New-CommandletArguments $commandletOut 'tunnelcoreprobe' ($Assets -eq 'owner') $false 128
            $commandletLedger = Invoke-UnrealLaunch 'probe-commandlet' $args `
                (Join-Path $RunRoot 'Logs\probe.log') (Join-Path $RunRoot 'Logs\probe.stdout.log') (Join-Path $RunRoot 'Logs\probe.stderr.log')
            $CommandletMetrics = Get-CommandletMetrics $commandletOut 'owner probe'
        }
        'perf' {
            $baseCvars['voxel.TestExitSeconds'] = 15
            $baseCvars['voxel.ProfileTileGeneration'] = 1
            $baseCvars['voxel.UseBlockEarlyOut'] = 0
            $baseCvars['voxel.OuterClassifierMode'] = 0
            $baseCvars['voxel.SealedSolidProof'] = 0
            $args = New-CommonArguments 'game' (Join-Path $RunRoot 'Logs\perf.log') (Merge-Cvars $baseCvars)
            $gameLedger = Invoke-UnrealLaunch 'perf-game' $args `
                (Join-Path $RunRoot 'Logs\perf.log') (Join-Path $RunRoot 'Logs\perf.stdout.log') (Join-Path $RunRoot 'Logs\perf.stderr.log')
            $GameMetrics = Get-StreamingMetrics (Join-Path $RunRoot 'Logs\perf.log')
        }
        'surface-fall' {
            $baseCvars['voxel.TestSurfaceFall'] = 1
            $baseCvars['voxel.TestSurfaceFallSpawnHeightVoxels'] = 20
            $baseCvars['voxel.TestSurfaceFallGroundToleranceVoxels'] = 4
            $baseCvars['voxel.TestSurfaceFallTimeoutSeconds'] = 8
            $args = New-CommonArguments 'game' (Join-Path $RunRoot 'Logs\surface-fall.log') (Merge-Cvars $baseCvars)
            $gameLedger = Invoke-UnrealLaunch 'surface-fall-game' $args `
                (Join-Path $RunRoot 'Logs\surface-fall.log') (Join-Path $RunRoot 'Logs\surface-fall.stdout.log') (Join-Path $RunRoot 'Logs\surface-fall.stderr.log')
            $SurfaceFallMetrics = Get-SurfaceFallMetrics (Join-Path $RunRoot 'Logs\surface-fall.log')
            if ($SurfaceFallMetrics.status -ne 'passed') {
                $surfaceReason = [string](Get-JsonValue $SurfaceFallMetrics 'reason' '')
                Add-Failure "surface-fall repro did not pass: $($SurfaceFallMetrics.status) $surfaceReason"
            }
        }
        'crossing' {
            $baseCvars['voxel.TestStrateCrossing'] = 1
            # The authored four-strate route is a real switchback whose vertical drop is hundreds
            # of metres. Use a fixed diagnostic speed and enough wall time for initial collision
            # cooking; the game code still uses swept character movement and the collision gate.
            $baseCvars['voxel.TestStrateCrossingSpeedCmPerSecond'] = 2000
            $baseCvars['voxel.TestStrateCrossingTimeoutSeconds'] = 360
            $baseCvars['voxel.TestStrateCrossingMaxGateSeconds'] = 10
            $args = New-CommonArguments 'game' (Join-Path $RunRoot 'Logs\crossing.log') (Merge-Cvars $baseCvars)
            $gameLedger = Invoke-UnrealLaunch 'strate-crossing-game' $args `
                (Join-Path $RunRoot 'Logs\crossing.log') (Join-Path $RunRoot 'Logs\crossing.stdout.log') (Join-Path $RunRoot 'Logs\crossing.stderr.log')
            $StrateCrossingMetrics = Get-StrateCrossingMetrics (Join-Path $RunRoot 'Logs\crossing.log')
            if ($StrateCrossingMetrics.status -ne 'passed') {
                $crossingReason = [string](Get-JsonValue $StrateCrossingMetrics 'reason' '')
                Add-Failure "strate-crossing test did not pass: $($StrateCrossingMetrics.status) $crossingReason"
            }
        }
        'gate-stress' {
            $baseCvars['voxel.TestCollisionGateStress'] = 1
            $baseCvars['voxel.TestCollisionGateStressSpeedCmPerSecond'] = 6000
            $baseCvars['voxel.TestCollisionGateStressStartDelaySeconds'] = 0.1
            $baseCvars['voxel.TestCollisionGateStressDurationSeconds'] = 2
            $baseCvars['voxel.TestCollisionGateStressTimeoutSeconds'] = 30
            $args = New-CommonArguments 'game' (Join-Path $RunRoot 'Logs\gate-stress.log') (Merge-Cvars $baseCvars)
            $gameLedger = Invoke-UnrealLaunch 'collision-gate-stress-game' $args `
                (Join-Path $RunRoot 'Logs\gate-stress.log') (Join-Path $RunRoot 'Logs\gate-stress.stdout.log') (Join-Path $RunRoot 'Logs\gate-stress.stderr.log')
            $CollisionGateStressMetrics = Get-CollisionGateStressMetrics (Join-Path $RunRoot 'Logs\gate-stress.log')
            if ($CollisionGateStressMetrics.status -ne 'passed') {
                $stressReason = [string](Get-JsonValue $CollisionGateStressMetrics 'reason' '')
                Add-Failure "collision-gate stress repro did not pass: $($CollisionGateStressMetrics.status) $stressReason"
            }
        }
        'tests' {
            $testPath = if ([string]::IsNullOrWhiteSpace($TestFilter)) { 'VoxelForge' } else { $TestFilter }
            $baseCvars['ExecCmds'] = "Automation RunTests $testPath; Quit"
            $baseCvars['ReportExportPath'] = (Join-Path $RunRoot 'AutomationReport')
            $args = New-CommonArguments 'tests' (Join-Path $RunRoot 'Logs\tests.log') (Merge-Cvars $baseCvars)
            # ExecCmds is a command-line value, not a cvar; replace the generated -ExecCmds token
            # with the quoted form accepted by Unreal's parser.
            $args = @($args | Where-Object { $_ -notlike '-ExecCmds=*' -and $_ -notlike '-ReportExportPath=*' })
            $args += ('-ExecCmds="Automation RunTests {0}; Quit"' -f $testPath)
            $args += ('-ReportExportPath="{0}"' -f (Join-Path $RunRoot 'AutomationReport'))
            $gameLedger = Invoke-UnrealLaunch 'automation-tests' $args `
                (Join-Path $RunRoot 'Logs\tests.log') (Join-Path $RunRoot 'Logs\tests.stdout.log') (Join-Path $RunRoot 'Logs\tests.stderr.log')
            $TestMetrics = @(Get-TestMetrics $gameLedger)
            if ($TestMetrics.Count -eq 0 -and $gameLedger.status -eq 'passed') {
                Add-Failure 'automation suite produced no per-test completion records'
            }
            $failedTests = @($TestMetrics | Where-Object { $_.status -eq 'failed' })
            if ($failedTests.Count -gt 0) {
                $failedNames = ($failedTests | ForEach-Object { $_.path }) -join ', '
                Add-Failure "automation tests failed: $failedNames"
            }

            if ([string]::IsNullOrWhiteSpace($TestFilter)) {
            # The unfiltered suite includes its collision integration repros. A filtered run is
            # bounded to its named automation tests and does not launch unrelated scenarios.
            $surfaceBaseCvars = [ordered]@{
                'voxel.TestSurfaceFall' = 1
                'voxel.TestSurfaceFallSpawnHeightVoxels' = 20
                'voxel.TestSurfaceFallGroundToleranceVoxels' = 4
                'voxel.TestSurfaceFallTimeoutSeconds' = 8
            }
            $surfaceArgs = New-CommonArguments 'game' (Join-Path $RunRoot 'Logs\surface-fall.log') (Merge-Cvars $surfaceBaseCvars)
            $surfaceFallLedger = Invoke-UnrealLaunch 'surface-fall-game' $surfaceArgs `
                (Join-Path $RunRoot 'Logs\surface-fall.log') (Join-Path $RunRoot 'Logs\surface-fall.stdout.log') (Join-Path $RunRoot 'Logs\surface-fall.stderr.log')
            $SurfaceFallMetrics = Get-SurfaceFallMetrics (Join-Path $RunRoot 'Logs\surface-fall.log')
            if ($SurfaceFallMetrics.status -ne 'passed') {
                $surfaceReason = [string](Get-JsonValue $SurfaceFallMetrics 'reason' '')
                Add-Failure "surface-fall repro did not pass: $($SurfaceFallMetrics.status) $surfaceReason"
            }

            $stressBaseCvars = [ordered]@{
                'voxel.TestCollisionGateStress' = 1
                'voxel.TestCollisionGateStressSpeedCmPerSecond' = 6000
                'voxel.TestCollisionGateStressStartDelaySeconds' = 0.1
                'voxel.TestCollisionGateStressDurationSeconds' = 2
                'voxel.TestCollisionGateStressTimeoutSeconds' = 30
            }
            $stressArgs = New-CommonArguments 'game' (Join-Path $RunRoot 'Logs\gate-stress.log') (Merge-Cvars $stressBaseCvars)
            $stressLedger = Invoke-UnrealLaunch 'collision-gate-stress-game' $stressArgs `
                (Join-Path $RunRoot 'Logs\gate-stress.log') (Join-Path $RunRoot 'Logs\gate-stress.stdout.log') (Join-Path $RunRoot 'Logs\gate-stress.stderr.log')
            $CollisionGateStressMetrics = Get-CollisionGateStressMetrics (Join-Path $RunRoot 'Logs\gate-stress.log')
            if ($CollisionGateStressMetrics.status -ne 'passed') {
                $stressReason = [string](Get-JsonValue $CollisionGateStressMetrics 'reason' '')
                Add-Failure "collision-gate stress repro did not pass: $($CollisionGateStressMetrics.status) $stressReason"
            }
            }
        }
        'parity' {
            $parityArgs = New-CommonArguments 'commandlet' (Join-Path $RunRoot 'Logs\parity-commandlet.log') (Merge-Cvars $baseCvars)
            # Use the same 128-voxel / 64-tile bounded region as the canonical exporter. The game
            # harness streams a larger clipmap around its center; comparison is restricted to these
            # exact level-0 tiles, including non-empty geometry rather than only an air tile.
            $parityArgs += New-CommandletArguments $commandletOut 'export' $true $true 128 0
            $commandletLedger = Invoke-UnrealLaunch 'parity-commandlet' $parityArgs `
                (Join-Path $RunRoot 'Logs\parity-commandlet.log') (Join-Path $RunRoot 'Logs\parity-commandlet.stdout.log') (Join-Path $RunRoot 'Logs\parity-commandlet.stderr.log')
            $CommandletMetrics = Get-CommandletMetrics $commandletOut 'owner assets; game_runtime_config=1'
            $parityJson = Read-JsonFile (Join-Path $commandletOut 'explore.json')
            if ($null -eq $parityJson) { throw 'Parity commandlet did not produce explore.json.' }
            $parityExport = Get-JsonValue $parityJson 'export'
            $region = Get-JsonValue $parityExport 'region_origin_voxels'
            if ($null -eq $region) { throw 'Parity commandlet did not report export.region_origin_voxels.' }
            $paritySize = [int](Get-JsonValue $parityExport 'region_size_cells_per_axis' 128)
            $centerOffset = [int]($paritySize / 2)
            $centerX = [int]$region.x + $centerOffset
            $centerY = [int]$region.y + $centerOffset
            $centerZ = [int]$region.z + $centerOffset
            $gameDump = Join-Path $RunRoot 'game_tile_hashes.json'
            $baseCvars['voxel.TestExitSeconds'] = 15
            $baseCvars['voxel.StreamingTestCenterX'] = $centerX
            $baseCvars['voxel.StreamingTestCenterY'] = $centerY
            $baseCvars['voxel.StreamingTestCenterZ'] = $centerZ
            $baseCvars['voxel.TileHashDump'] = $gameDump
            $baseCvars['voxel.UseBlockEarlyOut'] = 0
            $baseCvars['voxel.OuterClassifierMode'] = 0
            $baseCvars['voxel.SealedSolidProof'] = 0
            $gameArgs = New-CommonArguments 'game' (Join-Path $RunRoot 'Logs\parity-game.log') (Merge-Cvars $baseCvars)
            $gameLedger = Invoke-UnrealLaunch 'parity-game' $gameArgs `
                (Join-Path $RunRoot 'Logs\parity-game.log') (Join-Path $RunRoot 'Logs\parity-game.stdout.log') (Join-Path $RunRoot 'Logs\parity-game.stderr.log')
            $GameMetrics = Get-StreamingMetrics (Join-Path $RunRoot 'Logs\parity-game.log')
            $ParityEvidence = Compare-Parity $parityJson $gameDump `
                (Join-Path $RunRoot 'Logs\parity-commandlet.log') (Join-Path $RunRoot 'Logs\parity-game.log')
            if ($ParityEvidence.status -ne 'passed') {
                $difference = Get-JsonValue $ParityEvidence 'first_difference' $null
                if ($null -ne $difference) {
                    Add-Failure "parity verdict failed: first differing tile $($difference.tile) ($($difference.reason))"
                } else {
                    Add-Failure "parity verdict failed: $(Get-JsonValue $ParityEvidence 'reason' 'unknown parity failure')"
                }
            }
        }
    }
    $localAfter = Get-StateSnapshot $LocalAppDataRoot
    $LocalAppDataWrites = @(Get-StateChanges $localBefore $localAfter)
    if ($LocalAppDataWrites.Count -gt 0) {
        Add-Failure "writes detected under ${LocalAppDataRoot}: $($LocalAppDataWrites[0])"
    }
} catch {
    Add-Failure $_.Exception.Message
}

if ($null -eq $RunRoot) {
    $RunRoot = Join-Path $PluginRoot 'Saved\TestHarness\failed-before-output'
    Ensure-Directory $RunRoot
}
if ($null -eq $ResultPath) { $ResultPath = Join-Path $RunRoot 'result.json' }
if ($null -eq $SummaryPath) { $SummaryPath = Join-Path $RunRoot 'summary.txt' }

$pass = $Failures.Count -eq 0
$reason = if ($pass) { 'all requested checks passed' } else { [string]$Failures[0] }
$result = [ordered]@{
    schema_version = 1
    pass = $pass
    status = if ($pass) { 'PASS' } else { 'FAIL' }
    reason = $reason
    scenario = $Scenario
    label = $Label
    run_directory = $RunRoot
    project = $ProjectFile
    staged_host = $HostProject
    assets_mode = $Assets
    requested_cvars = $Cvars
    canonical_baseline = [ordered]@{
        commit = $CanonicalBaselineCommit
        expected_obj_sha256 = $CanonicalObjSha256
        checked = $Scenario -eq 'canonical'
    }
    assets = @($AssetEvidence)
    build = $BuildRecord
    staged_dlls = $DllEvidence
    capability_counts = if ($CommandletMetrics.Contains('capability_counts')) { $CommandletMetrics.capability_counts } else { [ordered]@{} }
    connectivity = if ($CommandletMetrics.Contains('connectivity')) { $CommandletMetrics.connectivity } else { [ordered]@{} }
    geometry_hash = if ($CommandletMetrics.Contains('geometry_hash')) { $CommandletMetrics.geometry_hash } else { $null }
    obj_sha256 = if ($CommandletMetrics.Contains('obj_sha256')) { $CommandletMetrics.obj_sha256 } else { $null }
    probe_amplitudes = if ($CommandletMetrics.Contains('probe_amplitudes')) { $CommandletMetrics.probe_amplitudes } else { [ordered]@{} }
    commandlet = $CommandletMetrics
    game = $GameMetrics
    surface_fall = $SurfaceFallMetrics
    strate_crossing = $StrateCrossingMetrics
    collision_gate_stress = $CollisionGateStressMetrics
    worker_seconds = [ordered]@{
        commandlet = if ($CommandletMetrics.Contains('worker_seconds')) { $CommandletMetrics.worker_seconds } else { $null }
        game = if ($GameMetrics.Contains('worker_generation_sum_s')) { $GameMetrics.worker_generation_sum_s } else { $null }
    }
    p50_p95 = if ($GameMetrics.Contains('p50_p95')) { $GameMetrics.p50_p95 } else { [ordered]@{} }
    tests = @($TestMetrics)
    parity = $ParityEvidence
    localappdata_unrealengine = [ordered]@{
        path = $LocalAppDataRoot
        writes_detected = @($LocalAppDataWrites)
        verified_clean = $LocalAppDataWrites.Count -eq 0
    }
    failures = @($Failures)
    launch_ledger = @($LaunchLedger)
}

try {
    Ensure-Directory (Split-Path -Parent $ResultPath)
    ($result | ConvertTo-Json -Compress -Depth 30) | Set-Content -LiteralPath $ResultPath -Encoding UTF8
    $summaryLines = [System.Collections.Generic.List[string]]::new()
    [void]$summaryLines.Add("$(if ($pass) { 'PASS' } else { 'FAIL' }) $reason")
    [void]$summaryLines.Add("scenario=$Scenario label=$Label")
    [void]$summaryLines.Add("run_directory=$RunRoot")
    [void]$summaryLines.Add("assets=$Assets")
    [void]$summaryLines.Add("canonical_baseline=$(ConvertTo-CompactJson $result.canonical_baseline)")
    [void]$summaryLines.Add("capability_counts=$(ConvertTo-CompactJson $result.capability_counts)")
    [void]$summaryLines.Add("connectivity=$(ConvertTo-CompactJson $result.connectivity)")
    [void]$summaryLines.Add("geometry_hash=$($result.geometry_hash)")
    [void]$summaryLines.Add("obj_sha256=$($result.obj_sha256)")
    [void]$summaryLines.Add("surface_fall=$(ConvertTo-CompactJson $result.surface_fall)")
    [void]$summaryLines.Add("strate_crossing=$(ConvertTo-CompactJson $result.strate_crossing)")
    [void]$summaryLines.Add("collision_gate_stress=$(ConvertTo-CompactJson $result.collision_gate_stress)")
    [void]$summaryLines.Add("probe_amplitudes=$(ConvertTo-CompactJson $result.probe_amplitudes)")
    [void]$summaryLines.Add("worker_seconds=$(ConvertTo-CompactJson $result.worker_seconds)")
    [void]$summaryLines.Add("p50_p95=$(ConvertTo-CompactJson $result.p50_p95)")
    [void]$summaryLines.Add("tests=$(ConvertTo-CompactJson $result.tests)")
    [void]$summaryLines.Add("parity=$(ConvertTo-CompactJson $result.parity)")
    [void]$summaryLines.Add("localappdata_unrealengine_clean=$($result.localappdata_unrealengine.verified_clean)")
    [void]$summaryLines.Add("runtime_dll=$($DllEvidence.runtime.sha256)")
    [void]$summaryLines.Add("editor_dll=$($DllEvidence.editor.sha256)")
    [void]$summaryLines.Add("unreal_launch_count=$($LaunchLedger.Count)")
    foreach ($entry in @($LaunchLedger)) {
        [void]$summaryLines.Add("launch kind=$($entry.kind) pid=$($entry.pid) status=$($entry.status) exit=$($entry.exit_code) timeout=$($entry.timed_out) crashes=$(@($entry.new_crashes).Count)")
    }
    foreach ($failure in @($Failures | Select-Object -Skip 1)) { [void]$summaryLines.Add("failure=$failure") }
    $summaryLines | Select-Object -First 40 | Set-Content -LiteralPath $SummaryPath -Encoding UTF8
} catch {
    # The primary result path is already in scope. A failed summary write must still make the
    # process non-zero; do not hide the original failure.
    Add-Failure "could not write result files: $($_.Exception.Message)"
}

if ($Failures.Count -gt 0) {
    exit 1
}
exit 0
