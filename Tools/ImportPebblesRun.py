"""Run the pebble import inside the full editor and quit afterwards."""
import runpy

import unreal

runpy.run_path('C:/Fish/Tools/ImportPebbles.py', run_name='__main__')
unreal.log('PEBBLE_IMPORT_DONE')
unreal.SystemLibrary.quit_editor()
