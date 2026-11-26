"""Run ULakeLevelLibrary::WaterAlignmentLog on the map the editor opened and quit.

The log gets the QUIET_LAKE_ALIGN lines: baked ground against the landscape, the bank transects,
the depths at the fishing spots. Tools/WorldBuild.ps1 runs this in the full editor and fails the
build when the mean disagreement is too large.

    UnrealEditor.exe C:/Fish/QuietLake.uproject -ExecCmds="py C:/Fish/Tools/WaterAlignment.py"
"""
import unreal

state = {'frames': 0, 'handle': None, 'done': False}


def tick(delta):
    state['frames'] += 1
    if state['frames'] < 40 or state['done']:
        return
    state['done'] = True
    try:
        unreal.LakeLevelLibrary.water_alignment_log()
    finally:
        unreal.unregister_slate_post_tick_callback(state['handle'])
        unreal.SystemLibrary.quit_editor()


state['handle'] = unreal.register_slate_post_tick_callback(tick)
