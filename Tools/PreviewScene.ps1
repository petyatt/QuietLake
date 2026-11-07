param(
    [string]$View = 'Camp',
    [switch]$StayOpen
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$enginePath = 'F:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
if ($View.Contains('"')) { throw 'View must not contain quotes.' }
$safeName = $View -replace '[^\p{L}\p{N}_-]', '_'
$logPath = Join-Path $projectRoot ('Saved\Logs\Scene_' + $safeName + '.log')
$arguments = @(
    ('"' + (Join-Path $projectRoot 'QuietLake.uproject') + '"'),
    '/Game/Maps/Lake', '-game', '-windowed', '-ResX=1280', '-ResY=720',
    ('-ScenePreview="' + $View + '"'), '-nosound', ('-abslog="' + $logPath + '"')
)
if ($StayOpen) { $arguments += '-PreviewStay' }
Start-Process -FilePath $enginePath -ArgumentList $arguments -WindowStyle Normal | Out-Null
Write-Output ('Direct scene preview: ' + $View + '; screenshot in Saved/Scene_' + $View + '.png')
