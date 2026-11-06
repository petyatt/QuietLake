"""Open the lake's plant graphs (Tools/CreateGrownPlants.py) in the Procedural Vegetation Editor, ready to
export: in each graph's window press Ctrl+E and confirm (the editor has no export call for scripts).

    UnrealEditor.exe C:/Fish/QuietLake.uproject -ExecCmds="py C:/Fish/Tools/ExportGrownPlants.py"

QUIETLAKE_PLANTS=Pine_A,Birch_B opens only those.
"""
import os

import unreal

NAMES = os.environ.get('QUIETLAKE_PLANTS', 'Pine_A,Pine_B,Spruce_A,Spruce_B,Birch_A,Birch_B,Alder_A,Shrub_A,Shrub_B').split(',')
state = {'frames': 0, 'handle': None}


def tick(delta):
    state['frames'] += 1
    if state['frames'] < 60:
        return
    unreal.unregister_slate_post_tick_callback(state['handle'])
    assets = [unreal.load_asset('/Game/Plants/PV/PV_' + name.strip()) for name in NAMES]
    assets = [a for a in assets if a]
    unreal.get_editor_subsystem(unreal.AssetEditorSubsystem).open_editor_for_assets(assets)
    unreal.log('QUIET_LAKE_PLANTS opened %d graphs for export' % len(assets))


state['handle'] = unreal.register_slate_post_tick_callback(tick)
