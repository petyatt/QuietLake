"""Import the ambient loops of the lake's places (Tools/GenerateLakeSounds.py) as looping sound waves
and the thunder (Tools/GenerateThunder.py) as one-shots.

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/ImportLakeSounds.py
"""
import os

import unreal

assets = unreal.AssetToolsHelpers.get_asset_tools()
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Loops of the places, then the one-shot thunder (Tools/GenerateThunder.py).
for name in ('A_Brook', 'A_Frogs', 'A_Cuckoo', 'A_ThunderNear', 'A_ThunderMid', 'A_ThunderFar'):
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', os.path.join(root, 'Tools', 'GeneratedAudio', name + '.wav'))
    task.set_editor_property('destination_path', '/Game/Audio')
    task.set_editor_property('automated', True)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('save', True)
    assets.import_asset_tasks([task])
    sound = unreal.load_asset('/Game/Audio/' + name)
    if not sound:
        raise RuntimeError('Audio import failed: ' + name)
    sound.set_editor_property('looping', not name.startswith('A_Thunder'))
    unreal.EditorAssetLibrary.save_asset('/Game/Audio/' + name)
unreal.log('QUIET_LAKE_SOUNDS_READY')
