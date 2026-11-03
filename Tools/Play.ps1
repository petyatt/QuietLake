param([ValidateSet('Solo', 'Host', 'Join')][string]$Mode = 'Solo', [string]$Address = '127.0.0.1')
$projectFile = Join-Path (Split-Path $PSScriptRoot -Parent) 'QuietLake.uproject'
$destination = '/Engine/Maps/Entry'
if ($Mode -eq 'Host') { $destination += '?listen' }
if ($Mode -eq 'Join') { $destination = $Address }
# Solo uses the resolution and window mode from the in-game settings; two local players need windows.
$window = if ($Mode -eq 'Solo') { @() } else { @('-windowed', '-ResX=1280', '-ResY=720') }
& 'F:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' $projectFile $destination -game @window
