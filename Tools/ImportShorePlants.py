"""Import the shore plants (Tools/GenerateShorePlants.py) into /Game/Plants/Shore (UE Python commandlet).

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/ImportShorePlants.py

Each gets M_GrassBlades (vertex colours, two-sided, swaying in the wind by the vertex alpha; made by
Tools/ImportPlants.py) and Nanite, as reed beds hold thousands of clumps. QUIETLAKE_SHORE=Reed_A,Sedge_B
imports only those.
"""
import os

import unreal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLANTS = ['Reed_A', 'Reed_B', 'Bulrush', 'Sedge_A', 'Sedge_B']
FOLDER = '/Game/Plants/Shore'
only = [n for n in os.environ.get('QUIETLAKE_SHORE', '').split(',') if n]
grass = unreal.load_asset('/Game/Plants/M_GrassBlades')
if not grass:
    raise RuntimeError('M_GrassBlades is missing: run Tools/ImportPlants.py first')
assets = unreal.AssetToolsHelpers.get_asset_tools()

for name in PLANTS:
    if only and name not in only:
        continue
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', os.path.join(ROOT, 'Tools', 'GeneratedPlants', 'SM_%s.fbx' % name))
    task.set_editor_property('destination_path', FOLDER)
    task.set_editor_property('automated', True)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('save', True)
    options = unreal.FbxImportUI()
    options.set_editor_property('import_mesh', True)
    options.set_editor_property('import_materials', False)
    options.set_editor_property('import_textures', False)
    options.set_editor_property('import_as_skeletal', False)
    sm = options.get_editor_property('static_mesh_import_data')
    sm.set_editor_property('combine_meshes', True)
    sm.set_editor_property('vertex_color_import_option', unreal.VertexColorImportOption.REPLACE)
    sm.set_editor_property('auto_generate_collision', False)
    sm.set_editor_property('normal_import_method', unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS)
    task.set_editor_property('options', options)
    assets.import_asset_tasks([task])
    path = FOLDER + '/SM_' + name
    mesh = unreal.load_asset(path)
    if not mesh:
        raise RuntimeError('Import failed: ' + name)
    mesh.set_material(0, grass)
    nanite = mesh.get_editor_property('nanite_settings')
    nanite.set_editor_property('enabled', True)
    mesh.set_editor_property('nanite_settings', nanite)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    b = mesh.get_bounds()
    unreal.log('QUIET_LAKE_SHORE_PLANT %s extent=(%.2f, %.2f, %.2f)' % (name, b.box_extent.x, b.box_extent.y, b.box_extent.z))
