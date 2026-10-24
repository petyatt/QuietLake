$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$engine = 'F:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$project = Join-Path $root 'QuietLake.uproject'
$logs = Join-Path $root ('Saved\NetworkTest\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$processes = @()
try {
    $serverLog = Join-Path $logs 'Server.log'
    $processes += Start-Process $engine -ArgumentList "`"$project`" /Engine/Maps/Entry -server -port=17777 -unattended -NullRHI -nosound -nop4 -abslog=`"$serverLog`"" -WindowStyle Hidden -PassThru
    $ready = $false
    for ($i = 0; $i -lt 90; $i++) {
        if ((Test-Path $serverLog) -and (Select-String -Path $serverLog -Pattern 'listening on port 17777' -Quiet)) { $ready = $true; break }
        if ($processes[0].HasExited) { throw 'Server exited before listening' }
        Start-Sleep -Seconds 1
    }
    if (-not $ready) { throw 'Server startup timed out' }
    Write-Output 'Server ready on local test port 17777'
    foreach ($number in 1,2) {
        $clientLog = Join-Path $logs "Client$number.log"
        $processes += Start-Process $engine -ArgumentList "`"$project`" 127.0.0.1:17777 -game -unattended -NullRHI -nosound -nop4 -abslog=`"$clientLog`"" -WindowStyle Hidden -PassThru
    }
    $joined = $false
    for ($i = 0; $i -lt 90; $i++) {
        $joins = @(Select-String -Path $serverLog -Pattern 'Join succeeded:')
        if ($joins.Count -ge 2) { $joined = $true; break }
        Start-Sleep -Seconds 1
    }
    if (-not $joined) { throw 'Two-client connection timed out; inspect Saved/NetworkTest logs' }
    Write-Output 'PASS: two clients joined the server'
    $joins | ForEach-Object { $_.Line }
} finally {
    foreach ($process in $processes) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    }
}
