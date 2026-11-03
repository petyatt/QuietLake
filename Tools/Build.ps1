$ErrorActionPreference = 'Stop'
$projectFile = Join-Path (Split-Path $PSScriptRoot -Parent) 'QuietLake.uproject'
& 'F:\UE_5.8\Engine\Build\BatchFiles\Build.bat' QuietLakeEditor Win64 Development "-Project=$projectFile" -WaitMutex
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
