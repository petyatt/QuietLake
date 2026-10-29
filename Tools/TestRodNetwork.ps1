$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$engine = 'F:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$logs = Join-Path $root ('Saved\RodNetwork\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$processes = @()
try {
    $project = Join-Path $root 'QuietLake.uproject'
    $serverLog = Join-Path $logs 'Server.log'
    $processes += Start-Process $engine -ArgumentList "`"$project`" /Engine/Maps/Entry -server -port=17779 -RodDiagnostics -NullRHI -unattended -nosound -abslog=`"$serverLog`"" -WindowStyle Hidden -PassThru
    for ($i=0; $i -lt 90; $i++) {
        if ((Test-Path $serverLog) -and (Select-String -Path $serverLog -Pattern 'listening on port 17779' -Quiet)) { break }
        if ($processes[0].HasExited) { throw 'Server exited' }
        Start-Sleep -Seconds 1
    }
    foreach ($number in 1,2) {
        $scenario = if ($number -eq 1) { '-RodNetworkTest' } else { '' }
        $clientLog = Join-Path $logs "Client$number.log"
        $processes += Start-Process $engine -ArgumentList "`"$project`" 127.0.0.1:17779 -game -RodDiagnostics $scenario -NullRHI -unattended -nosound -abslog=`"$clientLog`"" -WindowStyle Hidden -PassThru
    }
    Write-Output "Rod scenario logs: $logs"
    Start-Sleep -Seconds 65
    if (@(Select-String -Path $serverLog -Pattern 'Join succeeded:').Count -lt 2) { throw 'Two clients did not join' }
    foreach ($name in 'Server','Client1','Client2') {
        $path = Join-Path $logs "$name.log"
        if (-not (Select-String -Path $path -Pattern 'ROD_DIAG.*flying=1' -Quiet)) { throw "No physical flight observed on $name" }
        Write-Output "Physical flight observed on $name"
    }
    Write-Output 'Inspect matching cast times, positions and splashdown in the logs before declaring agreement.'
} finally {
    foreach ($process in $processes) { if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force } }
}
