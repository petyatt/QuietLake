param([ValidateSet('Solo', 'Host', 'Join')][string]$Mode = 'Solo', [string]$Address = '127.0.0.1')
$projectFile = Join-Path (Split-Path $PSScriptRoot -Parent) 'QuietLake.uproject'
$destination = '/Engine/Maps/Entry'
if ($Mode -eq 'Host') { $destination += '?listen' }
if ($Mode -eq 'Join') { $destination = $Address }
& 'F:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' $projectFile $destination -game -windowed -ResX=1280 -ResY=720
