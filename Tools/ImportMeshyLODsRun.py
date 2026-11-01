"""Run the Meshy LOD import inside the full editor (the commandlet has no static mesh subsystem)
and quit the editor afterwards.

    UnrealEditor.exe C:/Fish/QuietLake.uproject -ExecutePythonScript="C:/Fish/Tools/ImportMeshyLODsRun.py"

QUIETLAKE_FISH and QUIETLAKE_MESHY_SOURCE work as for ImportMeshyLODs.py.
"""
import runpy

import unreal

runpy.run_path('C:/Fish/Tools/ImportMeshyLODs.py', run_name='__main__')
unreal.log('MESHY_LOD_IMPORT_DONE')
unreal.SystemLibrary.quit_editor()
