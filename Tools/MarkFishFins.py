"""Mark the fins of every fish model in its vertex colours (ULakeFishTools::MarkFishFins).

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/MarkFishFins.py

Green = 1 - how much of a fin a vertex is, blue = which fin (1 body, 0.75 dorsal, 0.5 pectoral,
0.25 pelvic or anal, 0 the tail fin); red, the wave coordinate, is kept. The fish materials move the
fins by it (Tools/AddFishSpine.py). QUIETLAKE_FISH=Perch,Roach (environment): only these species.
"""
import os
import unreal

ALL = ['Bleak', 'Bream', 'Crucian', 'Perch', 'Pike', 'Roach', 'Rudd']
ONLY = [n.strip() for n in os.environ.get('QUIETLAKE_FISH', '').split(',') if n.strip()]
for species in ONLY or ALL:
    path = '/Game/Fish/SM_Fish_Meshy_' + species
    mesh = unreal.load_asset(path)
    if not mesh:
        unreal.log_warning('FISH_FINS no mesh ' + path)
        continue
    unreal.log('FISH_FINS ' + unreal.LakeFishTools.mark_fish_fins(mesh))
    unreal.EditorAssetLibrary.save_asset(path)
