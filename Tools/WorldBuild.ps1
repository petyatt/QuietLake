<#
Единый сценарий сборки мира: сборка C++ → уровень → вода → биомы → проверки.

    powershell -ExecutionPolicy Bypass -File Tools/WorldBuild.ps1
    powershell -ExecutionPolicy Bypass -File Tools/WorldBuild.ps1 -SkipBuild -NoBiomes
    powershell -ExecutionPolicy Bypass -File Tools/WorldBuild.ps1 -CheckOnly

Шаги запускают существующие скрипты: Build.bat, BuildLakeLevel.py, ApplyWaterMaterials.py,
CheckVillageWater.py, BuildLakeBiomes.py и WaterAlignment.py (только проверка выравнивания).
Логи шагов — в Saved/Logs/WorldBuild_<дата-время>/, сводка — summary.txt рядом.
Шаг без ожидаемых строк в логе считается проваленным: код выхода 1 и путь к логу.
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$NoWater,
    [switch]$NoBiomes,
    [switch]$RebuildWater,
    [switch]$CheckOnly,
    [bool]$Align = $true,
    [string]$LogRoot = ''
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$Uproject = Join-Path $ProjectRoot 'QuietLake.uproject'
$Engine = 'F:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$EngineCmd = 'F:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$BuildBat = 'F:\UE_5.8\Engine\Build\BatchFiles\Build.bat'
$ToolsUrl = 'C:/Fish/Tools/'
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrEmpty($LogRoot)) {
    $LogRoot = Join-Path $ProjectRoot ("Saved\Logs\WorldBuild_" + $Stamp)
}
New-Item -ItemType Directory -Force -Path $LogRoot | Out-Null
$script:Steps = @()
$script:Notes = New-Object System.Collections.Generic.List[string]

function Write-Note([string]$Text) {
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Text
    Write-Host $line
    $script:Notes.Add($line) | Out-Null
}

function Save-Summary {
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('WorldBuild ' + $Stamp) | Out-Null
    $lines.Add('project: ' + $Uproject) | Out-Null
    $lines.Add(('env: DEPTH={0} RAMP={1} SPACING={2} OUTWARD={3} WATER_INFO={4}' -f
        $env:QUIETLAKE_LAKE_DEPTH, $env:QUIETLAKE_LAKE_RAMP, $env:QUIETLAKE_LAKE_SPACING,
        $env:QUIETLAKE_LAKE_OUTWARD, $env:QUIETLAKE_WATER_INFO)) | Out-Null
    $lines.Add('') | Out-Null
    foreach ($step in $script:Steps) {
        $lines.Add(('{0,-4} {1,7:N1} s  {2}  {3}' -f $step.Status, $step.Seconds, $step.Name, $step.Log)) | Out-Null
        if ($step.Note) { $lines.Add('     ' + $step.Note) | Out-Null }
    }
    $lines.Add('') | Out-Null
    foreach ($note in $script:Notes) { $lines.Add($note) | Out-Null }
    $lines | Set-Content -Path (Join-Path $LogRoot 'summary.txt') -Encoding UTF8
}

function Invoke-Step {
    param([string]$Name, [scriptblock]$Body)
    Write-Note ('=== ' + $Name)
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $log = & $Body
    }
    catch {
        $watch.Stop()
        $script:Steps += [pscustomobject]@{
            Name = $Name; Status = 'FAIL'; Seconds = $watch.Elapsed.TotalSeconds; Log = ''; Note = $_.Exception.Message
        }
        Write-Note ('провал: ' + $Name + ' — ' + $_.Exception.Message)
        Save-Summary
        Write-Host ('сводка: ' + (Join-Path $LogRoot 'summary.txt'))
        exit 1
    }
    $watch.Stop()
    $script:Steps += [pscustomobject]@{
        Name = $Name; Status = 'ok'; Seconds = $watch.Elapsed.TotalSeconds; Log = [string]$log; Note = ''
    }
    Write-Note ('готово: ' + $Name + ' за ' + [math]::Round($watch.Elapsed.TotalSeconds, 1) + ' с')
}

function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments, [string]$LogFile, [int]$TimeoutMinutes, [string]$What)
    $process = Start-Process -FilePath $Exe -ArgumentList $Arguments -PassThru
    $limit = $TimeoutMinutes * 60 * 1000
    if (-not $process.WaitForExit($limit)) {
        try { $process.Kill() } catch { }
        throw ('{0}: таймаут {1} мин, процесс остановлен; лог {2}' -f $What, $TimeoutMinutes, $LogFile)
    }
    if ($process.ExitCode -ne 0) {
        throw ('{0}: код выхода {1}; лог {2}' -f $What, $process.ExitCode, $LogFile)
    }
    if (-not (Test-Path -LiteralPath $LogFile)) {
        throw ('{0}: лог не появился ({1})' -f $What, $LogFile)
    }
    return $LogFile
}

function Assert-Markers {
    param([string]$LogFile, [string[]]$Required, [string[]]$Forbidden = @())
    $text = Get-Content -LiteralPath $LogFile -Raw
    foreach ($marker in $Required) {
        if ($text.IndexOf($marker, [System.StringComparison]::Ordinal) -lt 0) {
            throw ('в логе нет «{0}»; лог {1}' -f $marker, $LogFile)
        }
    }
    foreach ($marker in $Forbidden) {
        if ($text.IndexOf($marker, [System.StringComparison]::Ordinal) -ge 0) {
            throw ('в логе есть «{0}»; лог {1}' -f $marker, $LogFile)
        }
    }
}

function Assert-EditorClosed {
    $editor = Get-Process -Name 'UnrealEditor' -ErrorAction SilentlyContinue
    if ($editor) {
        throw 'закройте Unreal Editor: сборка меняет карту и DLL'
    }
}

function Invoke-BuildStep {
    $log = Join-Path $LogRoot 'build.log'
    $arguments = @('QuietLakeEditor', 'Win64', 'Development', ('-Project=' + $Uproject), '-WaitMutex')
    $process = Start-Process -FilePath $BuildBat -ArgumentList $arguments -PassThru -RedirectStandardOutput $log -RedirectStandardError ($log + '.err')
    if (-not $process.WaitForExit(1800000)) {
        try { $process.Kill() } catch { }
        throw ('сборка: таймаут 30 мин; лог ' + $log)
    }
    if ($process.ExitCode -ne 0) {
        throw ('сборка: код выхода {0}; лог {1}' -f $process.ExitCode, $log)
    }
    Assert-Markers -LogFile $log -Required @('Result: Succeeded')
    return $log
}

function Invoke-EditorScript {
    param([string]$ScriptName, [string]$LogFile, [int]$TimeoutMinutes)
    $arguments = @($Uproject, ('-ExecCmds="py ' + $ToolsUrl + $ScriptName + '"'), ('-abslog=' + $LogFile))
    return Invoke-Tool -Exe $Engine -Arguments $arguments -LogFile $LogFile -TimeoutMinutes $TimeoutMinutes -What $ScriptName
}

function Invoke-Commandlet {
    param([string]$ScriptName, [string]$LogFile, [int]$TimeoutMinutes)
    $arguments = @($Uproject, '-run=pythonscript', ('-script=' + $ToolsUrl + $ScriptName),
        '-unattended', '-nopause', '-nosplash', '-NullRHI', ('-abslog=' + $LogFile))
    return Invoke-Tool -Exe $EngineCmd -Arguments $arguments -LogFile $LogFile -TimeoutMinutes $TimeoutMinutes -What $ScriptName
}

function Invoke-LevelStep {
    $log = Join-Path $LogRoot 'level.log'
    Invoke-EditorScript -ScriptName 'BuildLakeLevel.py' -LogFile $log -TimeoutMinutes 30 | Out-Null
    Assert-Markers -LogFile $log -Required @(
        'QUIET_LAKE_LEVEL waterline',
        'QUIET_LAKE_LEVEL lake:',
        'ground rebuilt from the design',
        'baked ',
        'heights to',
        'QUIET_LAKE_LEVEL done'
    ) -Forbidden @('QUIET_LAKE_LEVEL failed')
    return $log
}

function Invoke-WaterStep {
    if ($RebuildWater) {
        $material = Join-Path $LogRoot 'water_material.log'
        Invoke-Commandlet -ScriptName 'CreateVillageWater.py' -LogFile $material -TimeoutMinutes 15 | Out-Null
        $tuned = Join-Path $LogRoot 'water_tune.log'
        Invoke-Commandlet -ScriptName 'TuneVillageWater.py' -LogFile $tuned -TimeoutMinutes 15 | Out-Null
    }
    $log = Join-Path $LogRoot 'water_apply.log'
    Invoke-Commandlet -ScriptName 'ApplyWaterMaterials.py' -LogFile $log -TimeoutMinutes 15 | Out-Null
    Assert-Markers -LogFile $log -Required @('QUIET_LAKE_NATIVE_WATER saved')
    return $log
}

function Invoke-BiomesStep {
    $log = Join-Path $LogRoot 'biomes.log'
    Invoke-EditorScript -ScriptName 'BuildLakeBiomes.py' -LogFile $log -TimeoutMinutes 30 | Out-Null
    Assert-Markers -LogFile $log -Required @(
        'QUIET_LAKE_BIOMES baked',
        'QUIET_LAKE_BIOMES saved',
        'QUIET_LAKE_BIOMES done'
    ) -Forbidden @('QUIET_LAKE_BIOMES failed')
    return $log
}

function Invoke-WaterCheckStep {
    $log = Join-Path $LogRoot 'water_check.log'
    Invoke-Commandlet -ScriptName 'CheckVillageWater.py' -LogFile $log -TimeoutMinutes 15 | Out-Null
    Assert-Markers -LogFile $log -Required @('VILLAGE_WATER_CHECK passed')
    return $log
}

function Invoke-AlignmentStep {
    $log = Join-Path $LogRoot 'alignment.log'
    Invoke-EditorScript -ScriptName 'WaterAlignment.py' -LogFile $log -TimeoutMinutes 15 | Out-Null
    $text = Get-Content -LiteralPath $log -Raw
    $pattern = 'QUIET_LAKE_ALIGN baked ground vs landscape: (\d+) samples, mean ([0-9.]+) cm, (\d+) over 30 cm, worst ([0-9.]+) cm'
    $match = [regex]::Match($text, $pattern)
    if (-not $match.Success) {
        throw ('в логе нет строки QUIET_LAKE_ALIGN…; лог ' + $log)
    }
    $samples = [int]$match.Groups[1].Value
    $mean = [double]::Parse($match.Groups[2].Value, [System.Globalization.CultureInfo]::InvariantCulture)
    $over = [int]$match.Groups[3].Value
    $worst = [double]::Parse($match.Groups[4].Value, [System.Globalization.CultureInfo]::InvariantCulture)
    $share = if ($samples -gt 0) { $over / $samples } else { 1.0 }
    Write-Note ('выравнивание: среднее {0} см, {1} из {2} точек >30 см, худшее {3} см' -f $mean, $over, $samples, $worst)
    if ($mean -gt 10.0) {
        throw ('среднее расхождение высот {0} см больше 10 см; лог {1}' -f $mean, $log)
    }
    if ($share -gt 0.02) {
        throw ('доля точек с расхождением >30 см {0:P1} больше 2%; лог {1}' -f $share, $log)
    }
    return $log
}

try {
    $defaults = [ordered]@{
        QUIETLAKE_LAKE_DEPTH = '600'
        QUIETLAKE_LAKE_RAMP = '6000'
        QUIETLAKE_LAKE_SPACING = '150'
        QUIETLAKE_LAKE_OUTWARD = '150'
        QUIETLAKE_WATER_INFO = '2048'
    }
    foreach ($key in $defaults.Keys) {
        if ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($key))) {
            Set-Item -Path ('env:' + $key) -Value $defaults[$key]
        }
    }
    Write-Note ('логи: ' + $LogRoot)
    Write-Note ('env: DEPTH={0} RAMP={1} SPACING={2} OUTWARD={3} WATER_INFO={4}' -f
        $env:QUIETLAKE_LAKE_DEPTH, $env:QUIETLAKE_LAKE_RAMP, $env:QUIETLAKE_LAKE_SPACING,
        $env:QUIETLAKE_LAKE_OUTWARD, $env:QUIETLAKE_WATER_INFO)

    Invoke-Step 'редактор закрыт' { Assert-EditorClosed; '' }

    if (-not $CheckOnly) {
        if (-not $SkipBuild) {
            Invoke-Step 'сборка C++' { Invoke-BuildStep }
        }
        Invoke-Step 'уровень' { Invoke-LevelStep }
        if (-not $NoWater) {
            Invoke-Step 'вода: материалы' { Invoke-WaterStep }
        }
        if (-not $NoBiomes) {
            Invoke-Step 'биомы и растительность' { Invoke-BiomesStep }
        }
    }

    if (-not $NoWater) {
        Invoke-Step 'вода: проверка' { Invoke-WaterCheckStep }
    }
    if ($Align) {
        Invoke-Step 'выравнивание рельефа' { Invoke-AlignmentStep }
    }

    Save-Summary
    Write-Host ''
    Write-Host ('Сборка мира завершена. Сводка: ' + (Join-Path $LogRoot 'summary.txt'))
    exit 0
}
catch {
    Write-Note ('провал: ' + $_.Exception.Message)
    Save-Summary
    Write-Host ('Сводка: ' + (Join-Path $LogRoot 'summary.txt'))
    exit 1
}
