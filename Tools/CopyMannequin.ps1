# Copies the UE5 Manny/Quinn mannequins and their animations from the engine templates
# into Content/Characters/Mannequins (the path the template assets expect). Not stored in git.
$ErrorActionPreference = 'Stop'
$source = 'F:\UE_5.8\Templates\TemplateResources\High\Characters\Content\Mannequins'
$target = Join-Path (Split-Path $PSScriptRoot -Parent) 'Content\Characters\Mannequins'
New-Item -ItemType Directory -Force -Path $target | Out-Null
Copy-Item -Path (Join-Path $source '*') -Destination $target -Recurse -Force
Write-Output ('Mannequin assets copied: ' + (Get-ChildItem $target -Recurse -Filter *.uasset).Count)
