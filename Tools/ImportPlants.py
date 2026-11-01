"""Import the generated water plants and build their material (run as a UE Python commandlet).

M_Plant       - vertex colours, waxy leaf surface, two-sided (water lilies); the leaves and flowers
                ride the live waves near the viewer (ALakeRipples sets Ripples / RippleWindow).
M_GrassBlades - vertex colours, two-sided, swaying in the wind by vertex alpha (height fraction).

QUIET_LAKE_ONLY=M_Plant (environment variable): rebuild only M_Plant, in place (the imported
meshes keep it), and skip the grass and the import.
"""
import os
import unreal

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PARTS = ['LilyPad_A', 'LilyPad_B', 'LilyPad_C', 'LilyFlower', 'LilyYellow', 'Grass_Short', 'Grass_Tall', 'Sedge', 'Meadow_Flowers', 'Grass_Waist', 'Meadowsweet']
FOLDER = '/Game/Plants'


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def prop(src, out, p):
    if not lib.connect_material_property(src, out, p):
        raise RuntimeError('Cannot connect %s' % p)


only = [n for n in os.environ.get('QUIET_LAKE_ONLY', '').split(',') if n]

# Each vertex of a floating leaf or flower is lifted by the water under it, so a leaf rocks and
# bends over a passing wave. RW = (origin x, origin y, size, one texel in UV) of the wave window;
# outside it (and with the default zero window) the leaves lie still.
RIDE = """
float2 ruv = (P.xy - RW.xy) / max(RW.z, 1.0);
if (RW.z <= 0 || ruv.x < 0 || ruv.y < 0 || ruv.x > 1 || ruv.y > 1) return float3(0, 0, 0);
float edge = saturate(min(min(ruv.x, ruv.y), min(1.0 - ruv.x, 1.0 - ruv.y)) * 14.0);
return float3(0, 0, Texture2DSampleLevel(Rip, RipSampler, ruv, 0).r * edge);
"""

path = FOLDER + '/M_Plant'
if unreal.EditorAssetLibrary.does_asset_exist(path):
    # Rebuilt in place: the imported meshes keep pointing at it.
    mat = unreal.load_asset(path)
    lib.delete_all_material_expressions(mat)
else:
    mat = assets.create_asset('M_Plant', FOLDER, unreal.Material, unreal.MaterialFactoryNew())
mat.set_editor_property('two_sided', True)
mat.set_editor_property('used_with_instanced_static_meshes', True)
mat.set_editor_property('used_with_nanite', True)      # the lily meshes are Nanite
prop(node(mat, unreal.MaterialExpressionVertexColor), '', unreal.MaterialProperty.MP_BASE_COLOR)
prop(node(mat, unreal.MaterialExpressionConstant, r=0.38), '', unreal.MaterialProperty.MP_ROUGHNESS)
prop(node(mat, unreal.MaterialExpressionConstant, r=0.45), '', unreal.MaterialProperty.MP_SPECULAR)
ride = node(mat, unreal.MaterialExpressionCustom, code=RIDE, output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3)
items = []
for nm in ('P', 'Rip', 'RW'):
    it = unreal.CustomInput()
    it.set_editor_property('input_name', nm)
    items.append(it)
ride.set_editor_property('inputs', items)
where = node(mat, unreal.MaterialExpressionWorldPosition,
             world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
ripples = node(mat, unreal.MaterialExpressionTextureObjectParameter, parameter_name='Ripples',
               texture=unreal.load_asset('/Engine/EngineResources/Black'))
window = node(mat, unreal.MaterialExpressionVectorParameter, parameter_name='RippleWindow', default_value=unreal.LinearColor(0, 0, 0, 0))
for src, out, inp in ((where, '', 'P'), (ripples, '', 'Rip'), (window, 'RGBA', 'RW')):
    if not lib.connect_material_expressions(src, out, ride, inp):
        raise RuntimeError('link failed ' + inp)
prop(ride, '', unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
lib.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(path)
plant = mat
if only and 'M_Grass' not in only:
    unreal.log('QUIET_LAKE_PLANTS_READY (only M_Plant)')
    raise SystemExit

# Grass: gusts travel across the meadow; the tips move most.
path = FOLDER + '/M_GrassBlades'
if unreal.EditorAssetLibrary.does_asset_exist(path) and not unreal.EditorAssetLibrary.delete_asset(path):
    raise RuntimeError('Cannot delete ' + path)
grass = assets.create_asset('M_GrassBlades', FOLDER, unreal.Material, unreal.MaterialFactoryNew())
grass.set_editor_property('two_sided', True)
grass.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
grass.set_editor_property('used_with_instanced_static_meshes', True)
grass.set_editor_property('used_with_nanite', True)
vc = node(grass, unreal.MaterialExpressionVertexColor)
wp = node(grass, unreal.MaterialExpressionWorldPosition)
tm = node(grass, unreal.MaterialExpressionTime)
pos = node(grass, unreal.MaterialExpressionObjectPositionWS)
mpc = unreal.load_asset('/Game/Weather/MPC_Weather')
wnd, wdx, wdy, wet = (node(grass, unreal.MaterialExpressionCollectionParameter, collection=mpc, parameter_name=n) for n in ('Wind', 'WindDirX', 'WindDirY', 'Wetness'))
# The vertex colours are painted bright; real grass reflects far less light (albedo ~0.1-0.2), so
# they are scaled down here, and wet grass darkens further.
rgb = node(grass, unreal.MaterialExpressionCustom, code='return C * 0.60 * lerp(0.85, 1.1, frac(sin(dot(O.xy, float2(12.9898, 78.233))) * 43758.5453)) * lerp(1.0, 0.72, Wt);',
           output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3)
rough = node(grass, unreal.MaterialExpressionCustom, code='return lerp(0.7, 0.4, Wt);', output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT1)
sway = node(grass, unreal.MaterialExpressionCustom, output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,
            code='float speed = lerp(1.0, 2.6, W);\n'
                 'float gust = sin(T * 1.6 * speed - dot(P.xy, float2(X, Y)) * 0.004) * 0.6 + sin(T * 3.1 * speed + P.x * 0.011) * 0.25 + 0.5;\n'
                 'float amp = lerp(3.0, 26.0, W);\n'
                 'float h = A * A;\nreturn float3(float2(X, Y) * gust * amp + float2(-Y, X) * gust * amp * 0.25, -gust * amp * 0.15) * h;')
for n, names in ((rgb, ('C', 'O', 'Wt')), (rough, ('Wt',)), (sway, ('T', 'P', 'A', 'W', 'X', 'Y'))):
    items = []
    for nm in names:
        it = unreal.CustomInput()
        it.set_editor_property('input_name', nm)
        items.append(it)
    n.set_editor_property('inputs', items)
for src, out, dst, inp in ((vc, '', rgb, 'C'), (pos, '', rgb, 'O'), (wet, '', rgb, 'Wt'), (wet, '', rough, 'Wt'), (tm, '', sway, 'T'), (wp, '', sway, 'P'), (vc, 'A', sway, 'A'),
                           (wnd, '', sway, 'W'), (wdx, '', sway, 'X'), (wdy, '', sway, 'Y')):
    if not lib.connect_material_expressions(src, out, dst, inp):
        raise RuntimeError('link failed ' + inp)
prop(rgb, '', unreal.MaterialProperty.MP_BASE_COLOR)
prop(rgb, '', unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
prop(sway, '', unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
prop(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)
prop(node(grass, unreal.MaterialExpressionConstant, r=0.25), '', unreal.MaterialProperty.MP_SPECULAR)
lib.recompile_material(grass)
unreal.EditorAssetLibrary.save_asset(path)

for name in PARTS:
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
    mesh_path = FOLDER + '/SM_' + name
    mesh = unreal.load_asset(mesh_path)
    if not mesh:
        raise RuntimeError('Import failed: ' + name)
    mesh.set_material(0, plant if name.startswith('Lily') else grass)
    # Nanite: the beds hold tens of thousands of leaves.
    nanite = mesh.get_editor_property('nanite_settings')
    nanite.set_editor_property('enabled', True)
    mesh.set_editor_property('nanite_settings', nanite)
    unreal.EditorAssetLibrary.save_asset(mesh_path)
    b = mesh.get_bounds()
    unreal.log('PLANT_IMPORTED %s extent=(%.2f, %.2f, %.2f)' % (name, b.box_extent.x, b.box_extent.y, b.box_extent.z))
unreal.log('QUIET_LAKE_PLANTS_READY')
