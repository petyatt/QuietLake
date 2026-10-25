"""Import the generated fish models and build their material (run as a UE Python commandlet).

M_Fish: two-sided, colour from vertex colours, swimming done in World Position Offset.
Per-fish values come from Custom Primitive Data (no material instance per fish):
  0 = tail beat rate (rad/s), 1 = bend amplitude (fraction of length), 2 = phase, 3 = mesh length (cm).
"""
import os
import unreal

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPECIES = ['Perch', 'Roach', 'Crucian', 'Pike']
MAT = '/Game/Fish/M_Fish'


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def connect(src, out, dst, inp):
    if not lib.connect_material_expressions(src, out, dst, inp):
        raise RuntimeError('Material link failed: %s.%s -> %s.%s' % (src.get_name(), out, dst.get_name(), inp))


def custom(mat, code, inputs, output=unreal.CustomMaterialOutputType.CMOT_FLOAT3):
    n = node(mat, unreal.MaterialExpressionCustom, code=code, output_type=output)
    items = []
    for name, _ in inputs:
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        items.append(item)
    n.set_editor_property('inputs', items)
    for name, src in inputs:
        connect(src, '', n, name)
    return n


def prim_data(mat, name, index, default):
    p = node(mat, unreal.MaterialExpressionScalarParameter, parameter_name=name, default_value=default)
    p.set_editor_property('use_custom_primitive_data', True)
    p.set_editor_property('primitive_data_index', index)
    return p


# --- material ---
if unreal.EditorAssetLibrary.does_asset_exist(MAT) and not unreal.EditorAssetLibrary.delete_asset(MAT):
    raise RuntimeError('Cannot delete ' + MAT)
mat = assets.create_asset('M_Fish', '/Game/Fish', unreal.Material, unreal.MaterialFactoryNew())
mat.set_editor_property('two_sided', True)
vertex = node(mat, unreal.MaterialExpressionVertexColor)
uv = node(mat, unreal.MaterialExpressionTextureCoordinate)
time = node(mat, unreal.MaterialExpressionTime)
rate = prim_data(mat, 'SwimRate', 0, 8.0)
amp = prim_data(mat, 'SwimAmplitude', 1, 0.05)
phase = prim_data(mat, 'SwimPhase', 2, 0.0)
length = prim_data(mat, 'MeshLength', 3, 25.0)

# Lateral body wave in local space: travels head -> tail, grows towards the tail.
wave = custom(mat, """
float u = saturate(UV.x);
float env = 0.12 + pow(u, 1.7);
float side = A * Len * env * sin(u * 5.5 - T * R + Ph);
return float3(0, side, 0);
""", [('UV', uv), ('T', time), ('R', rate), ('A', amp), ('Ph', phase), ('Len', length)])
to_world = node(mat, unreal.MaterialExpressionTransform,
                transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_LOCAL,
                transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
connect(wave, '', to_world, '')   # single unnamed input
lib.connect_material_property(to_world, '', unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

# Scales: fine sparkle so the body does not look like plastic.
colour = custom(mat, """
float s = frac(sin(dot(floor(UV.xy * float2(220, 60)), float2(12.9898, 78.233))) * 43758.5453);
return V.rgb * (0.9 + 0.12 * s);
""", [('V', vertex), ('UV', uv)])
lib.connect_material_property(colour, '', unreal.MaterialProperty.MP_BASE_COLOR)
rough = node(mat, unreal.MaterialExpressionConstant, r=0.32)
lib.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)
spec = node(mat, unreal.MaterialExpressionConstant, r=0.65)
lib.connect_material_property(spec, '', unreal.MaterialProperty.MP_SPECULAR)
lib.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(MAT)

# --- meshes ---
for name in SPECIES:
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', os.path.join(ROOT, 'Tools', 'GeneratedFish', 'SM_Fish_%s.fbx' % name))
    task.set_editor_property('destination_path', '/Game/Fish')
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
    path = '/Game/Fish/SM_Fish_' + name
    mesh = unreal.load_asset(path)
    if not mesh:
        raise RuntimeError('Import failed: ' + name)
    mesh.set_material(0, unreal.load_asset(MAT))
    unreal.EditorAssetLibrary.save_asset(path)
    b = mesh.get_bounds()
    unreal.log('FISH_IMPORTED %s extent=(%.1f, %.1f, %.1f) origin=(%.1f, %.1f, %.1f)' % (
        name, b.box_extent.x, b.box_extent.y, b.box_extent.z, b.origin.x, b.origin.y, b.origin.z))
unreal.log('QUIET_LAKE_FISH_READY')
