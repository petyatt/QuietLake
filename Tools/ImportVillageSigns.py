"""Import the painted village signs (Tools/GeneratedSigns) and their material.

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/ImportVillageSigns.py
"""
import os
import unreal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'Tools', 'GeneratedSigns')
DEST = '/Game/Village/Signs'
NAMES = ['FishingShop', 'Store', 'Market', 'Fish', 'Tackle', 'Village']
tools = unreal.AssetToolsHelpers.get_asset_tools()
for name in NAMES:
    path = DEST + '/T_Sign_' + name
    if unreal.EditorAssetLibrary.does_asset_exist(path) and not unreal.EditorAssetLibrary.delete_asset(path):
        raise RuntimeError('Cannot delete ' + path)
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', os.path.join(SRC, 'T_Sign_%s.png' % name))
    task.set_editor_property('destination_path', DEST)
    task.set_editor_property('automated', True)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('save', True)
    tools.import_asset_tasks([task])

lib = unreal.MaterialEditingLibrary
MPATH = '/Game/Village/M_SignText'
if unreal.EditorAssetLibrary.does_asset_exist(MPATH) and not unreal.EditorAssetLibrary.delete_asset(MPATH):
    raise RuntimeError('Cannot delete ' + MPATH)
mat = tools.create_asset('M_SignText', '/Game/Village', unreal.Material, unreal.MaterialFactoryNew())
sample = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D)
sample.set_editor_property('parameter_name', 'Sign')
sample.set_editor_property('texture', unreal.load_asset(DEST + '/T_Sign_FishingShop'))
if not lib.connect_material_property(sample, 'RGB', unreal.MaterialProperty.MP_BASE_COLOR):
    raise RuntimeError('sign base colour link failed')
rough = lib.create_material_expression(mat, unreal.MaterialExpressionConstant)
rough.set_editor_property('r', 0.82)
if not lib.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS):
    raise RuntimeError('sign roughness link failed')
lib.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(MPATH)
print('QUIET_LAKE_SIGNS_READY')
