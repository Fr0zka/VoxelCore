param(
    [Parameter(Mandatory=$true)] [string] $RunName,
    [string] $TestName = 'VoxelForge.Determinism.DensityReuseRejectsMismatchedKey',
    [int] $HardLimitSeconds = 120
)

$ErrorActionPreference = 'Stop'
$pluginRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$hostRoot = Join-Path $pluginRoot 'Saved\BuildHost\HostProject'
$hostSaved = Join-Path $hostRoot 'Saved'
$uproject = Join-Path $hostRoot 'HostProject.uproject'
$editor = 'E:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe'
$stageBin = Join-Path $hostRoot 'Plugins\VoxelForge\Binaries\Win64'
$runtime = Join-Path $stageBin 'UnrealEditor-VoxelForge.dll'
$editorModule = Join-Path $stageBin 'UnrealEditor-VoxelForgeEditor.dll'
$crashRoot = Join-Path $hostRoot 'Saved\Crashes'
$ddcPath = Join-Path $pluginRoot 'Saved\DDC'
$zenPath = Join-Path $pluginRoot 'Saved\ZenData'
$runDir = Join-Path $pluginRoot ("Saved\PerfProbeR2_20260916\Automation\{0}" -f $RunName)
$logPath = Join-Path $runDir "$RunName.log"
$stdoutPath = Join-Path $runDir "$RunName.stdout.log"
$stderrPath = Join-Path $runDir "$RunName.stderr.log"
$modulePath = Join-Path $runDir "$RunName.LoadedModule.txt"

foreach ($path in @($uproject, $editor, $runtime, $editorModule)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing required file: $path" }
}
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
foreach ($path in @($logPath, $stdoutPath, $stderrPath, $modulePath)) {
    if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite launch artifact: $path" }
}
. (Join-Path $pluginRoot 'Saved\VoxelForgeHarnessCommon.ps1')

$expected = @{}
foreach ($path in @($runtime, $editorModule)) {
    $item = Get-Item -LiteralPath $path
    $expected[$item.Name] = [pscustomobject]@{
        Path = $item.FullName; Length = [int64]$item.Length
        SHA256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
    }
}
Set-Content -LiteralPath $modulePath -Encoding utf8 -Value @(
    "ExpectedRuntimePath=$($expected['UnrealEditor-VoxelForge.dll'].Path)"
    "ExpectedRuntimeLength=$($expected['UnrealEditor-VoxelForge.dll'].Length)"
    "ExpectedRuntimeSHA256=$($expected['UnrealEditor-VoxelForge.dll'].SHA256)"
    "ExpectedEditorPath=$($expected['UnrealEditor-VoxelForgeEditor.dll'].Path)"
    "ExpectedEditorLength=$($expected['UnrealEditor-VoxelForgeEditor.dll'].Length)"
    "ExpectedEditorSHA256=$($expected['UnrealEditor-VoxelForgeEditor.dll'].SHA256)"
    'LoadedModules='
)

$owners = @(Get-Process | Where-Object { $_.ProcessName -like 'UnrealEditor*' })
Write-Host "OWNER_EDITOR_CHECK launch=$RunName count=$($owners.Count)"
if ($owners.Count -ne 0) { throw "Refusing to launch while UnrealEditor* is running: $($owners.Id -join ',')" }
$startedUtc = (Get-Date).ToUniversalTime()
$crashBefore = Get-VoxelForgeCrashFolderSnapshot $crashRoot
$arguments = @(
    "`"$uproject`"", '-unattended', '-nop4', '-nosplash', '-nullrhi', '-NoZenAutoLaunch',
    "-ZenDataPath=`"$zenPath`"", '-ddc=NoZenLocalFallback', "-LocalDataCachePath=`"$ddcPath`"",
    '-nocrashreports', '-WindowsPlatformCrashContext.ForceCrashReportDialogOff=1',
    "-ExecCmds=`"Automation RunTests $TestName; Quit`"", "-abslog=`"$logPath`""
)
Write-Host "LAUNCH_START name=$RunName editor=$editor"
Write-Host "LAUNCH_COMMAND name=$RunName $editor $($arguments -join ' ')"
$process = Start-Process -FilePath $editor -ArgumentList $arguments -WorkingDirectory $hostRoot `
    -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -WindowStyle Hidden -PassThru
Write-Host "LAUNCH_PID name=$RunName pid=$($process.Id)"
$loadedNames = New-Object 'System.Collections.Generic.HashSet[string]'
$reporterPids = New-Object 'System.Collections.Generic.HashSet[int]'
$harnessError = ''
$watch = [System.Diagnostics.Stopwatch]::StartNew()

function Try-RecordLoadedModules(
    [System.Diagnostics.Process] $Process,
    [hashtable] $Expected,
    [string] $ModulePath,
    [System.Collections.Generic.HashSet[string]] $LoadedNames)
{
    try {
        $Process.Refresh()
        foreach ($module in $Process.Modules) {
            if (-not $Expected.ContainsKey($module.ModuleName) -or $LoadedNames.Contains($module.ModuleName)) { continue }
            $loadedPath = [System.IO.Path]::GetFullPath($module.FileName)
            $loadedItem = Get-Item -LiteralPath $loadedPath
            $loadedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $loadedPath).Hash
            Add-Content -LiteralPath $ModulePath -Encoding utf8 -Value @(
                "LoadedName=$($module.ModuleName)"
                "LoadedPath=$loadedPath"
                "LoadedLength=$($loadedItem.Length)"
                "LoadedSHA256=$loadedHash"
            )
            $expectedModule = $Expected[$module.ModuleName]
            if ($loadedPath -ine $expectedModule.Path -or [int64]$loadedItem.Length -ne $expectedModule.Length -or $loadedHash -ine $expectedModule.SHA256) {
                throw "Loaded module mismatch: $($module.ModuleName)"
            }
            [void]$LoadedNames.Add($module.ModuleName)
        }
    }
    catch [System.ComponentModel.Win32Exception] {
        # Process.Modules can be temporarily unavailable while the editor is initializing. The
        # log-side plugin marker below remains the required fallback for this editor-only launch.
    }
}

try {
    while (-not $process.HasExited) {
        [void](Stop-VoxelForgeCrashReporters -MonitoredPid $process.Id -SeenPids $reporterPids)
        Try-RecordLoadedModules -Process $process -Expected $expected -ModulePath $modulePath -LoadedNames $loadedNames
        if ($watch.Elapsed.TotalSeconds -ge $HardLimitSeconds) { throw "Automation run exceeded $HardLimitSeconds seconds." }
        Wait-Process -Id $process.Id -Timeout 1 -ErrorAction SilentlyContinue | Out-Null
    }
    $process.Refresh()
}
catch { $harnessError = $_.Exception.Message }
finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
}
for ($index = 0; $index -lt 8; ++$index) {
    [void](Stop-VoxelForgeCrashReporters -MonitoredPid $process.Id -SeenPids $reporterPids)
    Start-Sleep -Milliseconds 125
}
$process.Refresh()
$exitCode = [int]$process.ExitCode
$crashFolders = @(Get-VoxelForgeChangedCrashFolders -CrashRoot $crashRoot -Before $crashBefore -StartedUtc $startedUtc)
$testLines = @(Select-String -LiteralPath $logPath -Pattern ([regex]::Escape($TestName)) -ErrorAction SilentlyContinue)
$completionLines = @(Select-String -LiteralPath $logPath -Pattern 'TEST COMPLETE\. EXIT CODE:\s*0' -ErrorAction SilentlyContinue)
$failureLines = @(Select-String -LiteralPath $logPath -Pattern 'No automation tests matched|TEST COMPLETE\. EXIT CODE:\s*-[1-9]' -ErrorAction SilentlyContinue)
$pluginStartLines = @(Select-String -LiteralPath $logPath -Pattern 'VoxelForge module started' -ErrorAction SilentlyContinue)
$remainingReporters = @(Get-VoxelForgeCrashReporterProcesses -MonitoredPid $process.Id)
$ownerAfter = @(Get-Process | Where-Object { $_.ProcessName -like 'UnrealEditor*' })
$valid = $exitCode -eq 0 -and [string]::IsNullOrWhiteSpace($harnessError) `
    -and $testLines.Count -gt 0 -and $completionLines.Count -gt 0 -and $failureLines.Count -eq 0 `
    -and $pluginStartLines.Count -gt 0 -and $crashFolders.Count -eq 0 `
    -and $remainingReporters.Count -eq 0 -and $ownerAfter.Count -eq 0
Write-Host "OWNER_EDITOR_AFTER name=$RunName count=$($ownerAfter.Count)"
if ($crashFolders.Count -gt 0) { Write-Host "CRASH_FOLDERS name=$RunName $($crashFolders -join ';')" } else { Write-Host "CRASH_FOLDERS name=$RunName none" }
Write-Host "LAUNCH_RESULT name=$RunName outcome=$(if ($valid) { 'PASS' } else { 'FAIL' }) exit_code=$exitCode elapsed_seconds=$($watch.Elapsed.TotalSeconds.ToString('F6',[Globalization.CultureInfo]::InvariantCulture)) loaded_modules=$($loadedNames.Count)/2 plugin_marker=$($pluginStartLines.Count) crash_reporters_observed=$($reporterPids.Count)"
if ($harnessError) { Write-Host "LAUNCH_ERROR name=$RunName $harnessError" }
if ($testLines.Count -eq 0) { Write-Host "LAUNCH_FAILURE_REASON name=$RunName requested automation test was not found in log" }
if ($completionLines.Count -eq 0) { Write-Host "LAUNCH_FAILURE_REASON name=$RunName no successful automation completion marker was found" }
if ($failureLines.Count -gt 0) { Write-Host "LAUNCH_FAILURE_REASON name=$RunName automation reported a failure or no matching test" }
if ($pluginStartLines.Count -eq 0) { Write-Host "LAUNCH_FAILURE_REASON name=$RunName VoxelForge module startup marker was not observed" }
if ($crashFolders.Count -gt 0) { Write-Host "LAUNCH_FAILURE_REASON name=$RunName new crash folder observed" }
if ($ownerAfter.Count -ne 0) { Write-Host "LAUNCH_FAILURE_REASON name=$RunName UnrealEditor* remained after launch" }
Write-Host "LAUNCH_ARTIFACTS name=$RunName log=$logPath stdout=$stdoutPath stderr=$stderrPath loaded_modules=$modulePath"
if (-not $valid) { exit 1 }
exit 0
