"""Import the lake-bed weeds (Tools/GenerateLakeWeeds.py) into /Game/Plants/Weeds (UE Python commandlet).

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/ImportLakeWeeds.py

The weeds get M_WeedBlades: vertex colours darkened for the water, two-sided foliage shading, a slow
sway of the current by the vertex alpha (the height fraction), and Nanite on, as the beds hold thousands.
"""
import os

import unreal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = '/Game/Plants/Weeds'
PARTS = ['Weed_Ribbon', 'Weed_Grass', 'Weed_Elodea', 'Weed_Pondweed', 'Weed_Hornwort', 'Weed_Chara']
only = [n for n in os.environ.get('QUIETLAKE_WEEDS', '').split(',') if n]
assets = unreal.AssetToolsHelpers.get_asset_tools()
lib = unreal.MaterialEditingLibrary


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def connect(src, out, dst, inp):
    if not lib.connect_material_expressions(src, out, dst, inp):
        raise RuntimeError('link failed: ' + inp)


def prop(src, out, target):
    if not lib.connect_material_property(src, out, target):
        raise RuntimeError('property link failed: ' + str(target))


# The material: dark green blades lit softly, gently rocking in the current.
path = FOLDER + '/M_WeedBlades'
if unreal.EditorAssetLibrary.does_asset_exist(path) and not unreal.EditorAssetLibrary.delete_asset(path):
    raise RuntimeError('Cannot delete ' + path)
mat = assets.create_asset('M_WeedBlades', FOLDER, unreal.Material, unreal.MaterialFactoryNew())
mat.set_editor_property('two_sided', True)
mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
mat.set_editor_property('used_with_instanced_static_meshes', True)
mat.set_editor_property('used_with_nanite', True)
vc = node(mat, unreal.MaterialExpressionVertexColor)
wp = node(mat, unreal.MaterialExpressionWorldPosition)
tm = node(mat, unreal.MaterialExpressionTime)
mpc = unreal.load_asset('/Game/Weather/MPC_Weather')
wet = node(mat, unreal.MaterialExpressionCollectionParameter, collection=mpc, parameter_name='Wetness')
rgb = node(mat, unreal.MaterialExpressionCustom,
           code='return C * 0.55 * lerp(0.85, 1.05, frac(sin(dot(O.xy, float2(12.9898, 78.233))) * 43758.5453));',
           output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3)
sway = node(mat, unreal.MaterialExpressionCustom,
            code='float a = A * A;\nreturn float3(sin(T * 0.8 + P.x * 0.021) * 4.0 + sin(T * 1.7 + P.y * 0.017) * 1.5, '
                 'cos(T * 0.9 + P.y * 0.023) * 3.0, sin(T * 1.3 + P.x * 0.011) * 1.2) * a;',
            output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3)
for n, names in ((rgb, ('C', 'O')), (sway, ('T', 'P', 'A'))):
    items = []
    for nm in names:
        it = unreal.CustomInput()
        it.set_editor_property('input_name', nm)
        items.append(it)
    n.set_editor_property('inputs', items)
connect(vc, '', rgb, 'C')
connect(wp, '', rgb, 'O')
connect(tm, '', sway, 'T')
connect(wp, '', sway, 'P')
connect(vc, 'A', sway, 'A')
prop(rgb, '', unreal.MaterialProperty.MP_BASE_COLOR)
prop(rgb, '', unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
prop(sway, '', unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
prop(node(mat, unreal.MaterialExpressionConstant, r=0.86), '', unreal.MaterialProperty.MP_ROUGHNESS)
prop(node(mat, unreal.MaterialExpressionConstant, r=0.12), '', unreal.MaterialProperty.MP_SPECULAR)
lib.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(path)

for name in PARTS:
    if only and name not in only:
        continue
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', os.path.join(ROOT, 'Tools', 'GeneratedLakeWeeds', 'SM_%s.fbx' % name))
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
    asset_path = FOLDER + '/SM_' + name
    mesh = unreal.load_asset(asset_path)
    if not mesh:
        raise RuntimeError('Import failed: ' + name)
    mesh.set_material(0, mat)
    nanite = mesh.get_editor_property('nanite_settings')
    nanite.set_editor_property('enabled', True)
    mesh.set_editor_property('nanite_settings', nanite)
    unreal.EditorAssetLibrary.save_asset(asset_path, only_if_is_dirty=False)
    b = mesh.get_bounds()
    unreal.log('QUIET_LAKE_WEED %s extent=(%.2f, %.2f, %.2f)' % (name, b.box_extent.x, b.box_extent.y, b.box_extent.z))
print('QUIET_LAKE_WEEDS_READY')
