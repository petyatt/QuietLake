"""Weather assets (run as a UE Python commandlet, before CreateWorldAssets.py, CreateTerrainMaterial.py
and ImportPlants.py, which read MPC_Weather).

MPC_Weather - parameters the game sets every frame (ALakeWeather): Rain 0..1, Wind 0..1, WindDirX/Y
              (unit vector the wind blows towards), Wetness 0..1 (ground and wood after rain),
              Daylight 0..1 (for unlit effects such as rain at night).
M_Rain      - translucent streaks; falls, slants with the wind and thins out with the rain intensity.
SM_RainTile - the 12 m rain tile from GenerateWeatherModels.py.
"""
import os
import unreal

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = '/Game/Weather'
PARAMS = [('Rain', 0.0), ('Wind', 0.2), ('WindDirX', 1.0), ('WindDirY', 0.0), ('Wetness', 0.0), ('Daylight', 1.0)]

path = FOLDER + '/MPC_Weather'
mpc = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
if not mpc:
    mpc = assets.create_asset('MPC_Weather', FOLDER, unreal.MaterialParameterCollection, unreal.MaterialParameterCollectionFactoryNew())
existing = [str(p.get_editor_property('parameter_name')) for p in mpc.get_editor_property('scalar_parameters')]
if existing != [n for n, _ in PARAMS]:
    params = []
    for name, default in PARAMS:
        p = unreal.CollectionScalarParameter()
        p.set_editor_property('parameter_name', name)
        p.set_editor_property('default_value', default)
        params.append(p)
    mpc.set_editor_property('scalar_parameters', params)
unreal.EditorAssetLibrary.save_asset(path)


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def custom(mat, code, inputs, output):
    n = node(mat, unreal.MaterialExpressionCustom, code=code, output_type=output)
    items = []
    for name, _, _ in inputs:
        it = unreal.CustomInput()
        it.set_editor_property('input_name', name)
        items.append(it)
    n.set_editor_property('inputs', items)
    for name, src, pin in inputs:
        if not lib.connect_material_expressions(src, pin, n, name):
            raise RuntimeError('link failed ' + name)
    return n


def prop(src, p):
    if not lib.connect_material_property(src, '', p):
        raise RuntimeError('Cannot connect %s' % p)


def weather(mat, name):
    return node(mat, unreal.MaterialExpressionCollectionParameter, collection=mpc, parameter_name=name)


path = FOLDER + '/M_Rain'
if unreal.EditorAssetLibrary.does_asset_exist(path) and not unreal.EditorAssetLibrary.delete_asset(path):
    raise RuntimeError('Cannot delete ' + path)
mat = assets.create_asset('M_Rain', FOLDER, unreal.Material, unreal.MaterialFactoryNew())
mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
mat.set_editor_property('two_sided', True)
mat.set_editor_property('used_with_instanced_static_meshes', True)
vc = node(mat, unreal.MaterialExpressionVertexColor)
tm = node(mat, unreal.MaterialExpressionTime)
wp = node(mat, unreal.MaterialExpressionWorldPosition)
cam = node(mat, unreal.MaterialExpressionCameraPositionWS)
rain, wind, wx, wy, day = (weather(mat, n) for n in ('Rain', 'Wind', 'WindDirX', 'WindDirY', 'Daylight'))
F1, F3 = unreal.CustomMaterialOutputType.CMOT_FLOAT1, unreal.CustomMaterialOutputType.CMOT_FLOAT3
wpo = custom(mat, """
float H = 1400.0;
float v = 850.0 + 250.0 * C.r;
float f = frac(C.b - T * v / H + C.r);
float dz = (f - C.b) * H;
float2 dir = float2(X, Y);
float2 drift = dir * W * ((1.0 - f) * 380.0 + A * 35.0);
return float3(drift, dz);""", [('C', vc, ''), ('A', vc, 'A'), ('T', tm, ''), ('X', wx, ''), ('Y', wy, ''), ('W', wind, '')], F3)
prop(wpo, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
opacity = custom(mat, """
if (C.g > R) return 0;
float d = distance(P, Cam);
float fade = saturate((d - 150.0) / 400.0) * (1.0 - saturate((d - 1100.0) / 500.0));
return fade * 0.15 * (0.4 + 0.6 * R);""", [('C', vc, ''), ('R', rain, ''), ('P', wp, ''), ('Cam', cam, '')], F1)
prop(opacity, unreal.MaterialProperty.MP_OPACITY)
colour = custom(mat, 'return float3(0.5, 0.53, 0.56) * (0.06 + 0.94 * D);', [('D', day, '')], F3)
prop(colour, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
lib.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(path)

task = unreal.AssetImportTask()
task.set_editor_property('filename', os.path.join(ROOT, 'Tools', 'GeneratedWeather', 'SM_RainTile.fbx'))
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
task.set_editor_property('options', options)
assets.import_asset_tasks([task])
mesh = unreal.load_asset(FOLDER + '/SM_RainTile')
if not mesh:
    raise RuntimeError('Rain tile import failed')
mesh.set_material(0, mat)
unreal.EditorAssetLibrary.save_asset(FOLDER + '/SM_RainTile')
unreal.log('QUIET_LAKE_WEATHER_ASSETS_READY')
