"""Import the shore plants (Tools/GenerateShorePlants.py) into /Game/Plants/Shore (UE Python commandlet).

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/ImportShorePlants.py

The ground plants get M_GrassBlades (vertex colours, two-sided, swaying in the wind by the vertex
alpha), the floating lilies M_Plant (they ride the live waves); both made by Tools/ImportPlants.py. All
are Nanite, as the beds and meadows hold thousands of clumps. QUIETLAKE_SHORE=Reed_A,Sedge_B imports
only those.
"""
import os

import unreal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLANTS = ['Reed_A', 'Reed_B', 'Bulrush', 'Sedge_A', 'Sedge_B', 'Grass_A', 'Grass_B', 'Grass_Low', 'Lawn_A', 'Lawn_B', 'Flowers_A', 'Flowers_B',
          'LilyPads', 'LilyWhite', 'LilyNuphar']
FOLDER = '/Game/Plants/Shore'
only = [n for n in os.environ.get('QUIETLAKE_SHORE', '').split(',') if n]
grass = unreal.load_asset('/Game/Plants/M_GrassBlades')
floating = unreal.load_asset('/Game/Plants/M_Plant')
if not grass or not floating:
    raise RuntimeError('M_GrassBlades or M_Plant is missing: run Tools/ImportPlants.py first')
assets = unreal.AssetToolsHelpers.get_asset_tools()


def lawn_material():
    lib = unreal.MaterialEditingLibrary
    path = FOLDER + '/M_VillageLawn'
    if not unreal.EditorAssetLibrary.does_asset_exist(path):
        if not unreal.EditorAssetLibrary.duplicate_asset('/Game/Plants/M_GrassBlades', path):
            raise RuntimeError('Could not create the lawn material')
    mat = unreal.load_asset(path)
    colour = lib.get_material_property_input_node(mat, unreal.MaterialProperty.MP_BASE_COLOR)
    colour.set_editor_property('code', 'return C * 1.04 * lerp(0.88, 1.12, frac(sin(dot(O.xy, float2(12.9898, 78.233))) * 43758.5453)) * lerp(1.0, 0.72, Wt);')
    wind = lib.get_material_property_input_node(mat, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    wind.set_editor_property('code', wind.get_editor_property('code').replace('lerp(3.0, 26.0, W)', 'lerp(0.9, 5.0, W)'))
    mat.set_editor_property('tangent_space_normal', False)
    normal = lib.get_material_property_input_node(mat, unreal.MaterialProperty.MP_NORMAL)
    if normal is None:
        normal = lib.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector)
    normal.set_editor_property('constant', unreal.LinearColor(0.0, 0.0, 1.0, 1.0))
    if not lib.connect_material_property(normal, '', unreal.MaterialProperty.MP_NORMAL):
        raise RuntimeError('Could not set the lawn normal')
    lib.recompile_material(mat)
    if not unreal.EditorAssetLibrary.save_loaded_asset(mat):
        raise RuntimeError('Could not save the lawn material')
    return mat


lawn = lawn_material() if not only or any(name.startswith('Lawn_') for name in only) else None

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
    mesh.set_material(0, floating if name.startswith('Lily') else lawn if name.startswith('Lawn_') else grass)
    nanite = mesh.get_editor_property('nanite_settings')
    nanite.set_editor_property('enabled', True)
    mesh.set_editor_property('nanite_settings', nanite)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    b = mesh.get_bounds()
    unreal.log('QUIET_LAKE_SHORE_PLANT %s extent=(%.2f, %.2f, %.2f)' % (name, b.box_extent.x, b.box_extent.y, b.box_extent.z))
