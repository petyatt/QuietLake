$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$engine = 'F:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$project = Join-Path $root 'QuietLake.uproject'
$logs = Join-Path $root ('Saved\NetworkTest\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$processes = @()
try {
    $serverLog = Join-Path $logs 'Server.log'
    $processes += Start-Process $engine -ArgumentList "`"$project`" /Game/Maps/Lake -server -port=17777 -unattended -NullRHI -nosound -nop4 -abslog=`"$serverLog`"" -WindowStyle Hidden -PassThru
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
    # Fish are simulated on the server; each client must receive them.
    $seen = $false
    for ($i = 0; $i -lt 60; $i++) {
        $counts = foreach ($number in 1,2) {
            $line = Select-String -Path (Join-Path $logs "Client$number.log") -Pattern 'QUIET_LAKE_FISH_VISIBLE (\d+)' | Select-Object -Last 1
            if ($line) { [int]$line.Matches[0].Groups[1].Value } else { 0 }
        }
        if (($counts | Where-Object { $_ -gt 0 }).Count -eq 2) { $seen = $true; break }
        Start-Sleep -Seconds 1
    }
    if (-not $seen) { throw 'Fish did not replicate to both clients; inspect Saved/NetworkTest logs' }
    Write-Output ('PASS: both clients see fish: ' + ($counts -join ', '))
    # Fish swim smoothly on the clients: no steps between network updates (ALakeLife::LogShownMotion).
    $smooth = $false
    for ($i = 0; $i -lt 90; $i++) {
        $lines = foreach ($number in 1,2) {
            Select-String -Path (Join-Path $logs "Client$number.log") -Pattern 'QUIET_LAKE_FISH_SMOOTH .* accel p50=(\d+) p99=(\d+) .* turn-accel p50=(\d+) p99=(\d+)' | Select-Object -Last 1
        }
        if (@($lines | Where-Object { $_ }).Count -eq 2) { $smooth = $true; break }
        Start-Sleep -Seconds 1
    }
    if (-not $smooth) { throw 'Clients logged no fish motion; inspect Saved/NetworkTest logs' }
    foreach ($line in $lines) {
        Write-Output ('Client fish motion: ' + ($line.Line -replace '^.*QUIET_LAKE_FISH_SMOOTH ', ''))
        $p99 = [int]$line.Matches[0].Groups[2].Value
        $turn99 = [int]$line.Matches[0].Groups[4].Value
        if ($p99 -gt 600) { throw "Fish motion on a client is jerky (p99 acceleration $p99 cm/s2)" }
        if ($turn99 -gt 1500) { throw "Fish turns on a client are jerky (p99 turn acceleration $turn99 deg/s2)" }
    }
    Write-Output 'PASS: fish swim smoothly on both clients'
    # Each client sees the other angler start with empty hands: the case on his back, the three rods in
    # their compartments 0/1/2 (ALakeCharacter::PossessedBy stows them on the server, QUIET_LAKE_CASE_SEEN).
    $cases = $null
    for ($i = 0; $i -lt 30; $i++) {
        $cases = foreach ($number in 1,2) {
            Select-String -Path (Join-Path $logs "Client$number.log") -Pattern 'QUIET_LAKE_CASE_SEEN .* case=(\d+) rods=(\d+)/(\d+)/(\d+) slots=(-?\d+)/(-?\d+)/(-?\d+)' | Select-Object -Last 1
        }
        if (@($cases | Where-Object { $_ }).Count -eq 2) { break }
        Start-Sleep -Seconds 1
    }
    if (@($cases | Where-Object { $_ }).Count -ne 2) { throw 'A client logged no other angler''s rod case; inspect Saved/NetworkTest logs' }
    foreach ($line in $cases) {
        Write-Output ('Client sees: ' + ($line.Line -replace '^.*QUIET_LAKE_CASE_SEEN ', ''))
        $g = $line.Matches[0].Groups
        if ($g[1].Value -ne '0' -or $g[2].Value -ne '0' -or $g[3].Value -ne '0' -or $g[4].Value -ne '0' -or $g[5].Value -ne '0' -or $g[6].Value -ne '1' -or $g[7].Value -ne '2') {
            throw 'The other angler''s case and rods did not replicate as expected'
        }
    }
    Write-Output 'PASS: both clients see the other angler start with all rods in the case'
} finally {
    foreach ($process in $processes) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    }
}
