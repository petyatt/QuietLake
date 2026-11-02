"""Run the bottom props import inside the full editor and quit afterwards."""
import runpy

import unreal

runpy.run_path('C:/Fish/Tools/ImportBottomProps.py', run_name='__main__')
unreal.log('BOTTOM_PROPS_IMPORT_DONE')
unreal.SystemLibrary.quit_editor()
