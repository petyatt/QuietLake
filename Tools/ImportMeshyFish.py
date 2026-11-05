"""Import a textured fish model (a bought one, e.g. Meshy) and build its material.

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript \
        -script=C:/Fish/Tools/ImportTexturedFish.py

QUIETLAKE_FISH=Crucian (environment): which species; QUIETLAKE_MESHY_FOLDER=<folder with the
textures>. The mesh comes from Tools/GeneratedFish/SM_Fish_<Species>.fbx, processed by
ProcessMeshyFish.py (nose at the origin, body along +X, wave in vertex red).

M_Fish_<Species> keeps what M_Fish does — the swim wave and the bend in World Position Offset from
Custom Primitive Data — but takes the colour, the normal and the roughness from the model's own
textures and reads the wave coordinate from the vertex red channel.
"""
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unreal
import FishLook

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPECIES = os.environ.get('QUIETLAKE_FISH', 'Crucian').strip() or 'Crucian'
FOLDER = os.environ.get('QUIETLAKE_MESHY_FOLDER', '').strip()
ASSET_NAME = 'SM_Fish_Meshy_' + SPECIES
MESH_PATH = '/Game/Fish/' + ASSET_NAME
MAT_PATH = '/Game/Fish/M_Meshy_' + SPECIES
TEX_DIR = '/Game/Fish/Textures'


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


def import_texture(path, name, srgb, normal=False):
    if unreal.EditorAssetLibrary.does_asset_exist('%s/%s' % (TEX_DIR, name)):
        unreal.EditorAssetLibrary.delete_asset('%s/%s' % (TEX_DIR, name))
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', path)
    task.set_editor_property('destination_path', TEX_DIR)
    task.set_editor_property('destination_name', name)
    task.set_editor_property('automated', True)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('save', True)
    assets.import_asset_tasks([task])
    tex = unreal.load_asset('%s/%s' % (TEX_DIR, name))
    if not tex:
        raise RuntimeError('Texture import failed: ' + path)
    tex.set_editor_property('srgb', srgb)
    if normal:
        tex.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_NORMALMAP)
        tex.set_editor_property('flip_green_channel', True)
    tex.set_editor_property('lod_group', unreal.TextureGroup.TEXTUREGROUP_WORLD)
    unreal.EditorAssetLibrary.save_asset(tex.get_path_name())
    return tex


def texture_node(mat, tex, name, sampler):
    n = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name=name, texture=tex)
    n.set_editor_property('sampler_type', sampler)
    return n


base_name = 'T_Meshy_%s_Base' % SPECIES
normal_name = 'T_Meshy_%s_Normal' % SPECIES
rough_name = 'T_Meshy_%s_Rough' % SPECIES
tex_base = import_texture(str(next(Path(FOLDER).glob('*-texture.png'))),
                          base_name, True)
tex_normal = import_texture(str(next(Path(FOLDER).glob('*_normal.png'))),
                            normal_name, False, normal=True)
tex_rough = import_texture(str(next(Path(FOLDER).glob('*_roughness.png'))),
                           rough_name, False)

if unreal.EditorAssetLibrary.does_asset_exist(MAT_PATH):
    unreal.EditorAssetLibrary.delete_asset(MAT_PATH)
mat = assets.create_asset('M_Meshy_' + SPECIES, '/Game/Fish', unreal.Material, unreal.MaterialFactoryNew())
mat.set_editor_property('two_sided', True)

vcol = node(mat, unreal.MaterialExpressionVertexColor)
time = node(mat, unreal.MaterialExpressionTime)
rate = prim_data(mat, 'SwimRate', 0, 8.0)
amp = prim_data(mat, 'SwimAmplitude', 1, 0.05)
phase = prim_data(mat, 'SwimPhase', 2, 0.0)
length = prim_data(mat, 'MeshLength', 3, 40.0)
curl = prim_data(mat, 'SwimCurl', 4, 0.0)
wave = custom(mat, """
    float u = saturate(V.r);
    float env = pow(u, 1.7);
    float side = A * Len * env * sin(u * 5.5 - T * R + Ph);
    float d = u - 0.4;
    side += C * Len * (d * d - 0.16);
    return float3(0, side, 0);
    """, [('V', vcol), ('T', time), ('R', rate), ('A', amp), ('Ph', phase), ('Len', length), ('C', curl)])
to_world = node(mat, unreal.MaterialExpressionTransform,
                transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_LOCAL,
                transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
connect(wave, '', to_world, '')
lib.connect_material_property(to_world, '', unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

colour = texture_node(mat, tex_base, 'BaseColour', unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
normal = texture_node(mat, tex_normal, 'NormalMap', unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
lib.connect_material_property(normal, 'RGB', unreal.MaterialProperty.MP_NORMAL)
rough = texture_node(mat, tex_rough, 'Roughness', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
# Colour, roughness and specular: the fish's own above the water, darker and duller under it.
FishLook.apply(mat, colour, 'RGB', rough, 'R')
lib.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(MAT_PATH)

# The mesh itself, imported the same way as the procedural fish.
task = unreal.AssetImportTask()
task.set_editor_property('filename', os.path.join(ROOT, 'Tools', 'GeneratedFish', 'Meshy', ASSET_NAME + '_LOD0.fbx'))
task.set_editor_property('destination_path', '/Game/Fish')
task.set_editor_property('destination_name', ASSET_NAME)
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
mesh = unreal.load_asset(MESH_PATH)
if not mesh:
    raise RuntimeError('Mesh import failed: ' + MESH_PATH)
mesh.set_material(0, unreal.load_asset(MAT_PATH))
sub = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
for lod in (1, 2):
    source = os.path.join(ROOT, 'Tools', 'GeneratedFish', 'Meshy', ASSET_NAME + '_LOD%d.fbx' % lod)
    result = sub.import_lod(mesh, lod, source)
    if result != lod: raise RuntimeError('LOD import failed: ' + str(result))
mesh.set_material(0, mat)
assert sub.get_lod_count(mesh) == 3
sub.set_lod_screen_sizes(mesh, [1.0, 0.12, 0.025])
for lod in range(3):
    unreal.log('MESHY_LOD %s %d vertices=%d sections=%d' % (SPECIES, lod, mesh.get_num_vertices(lod), len(mesh.get_editor_property('static_materials'))))
unreal.EditorAssetLibrary.save_asset(MESH_PATH)
b = mesh.get_bounds()
unreal.log('TEXTURED_FISH %s extent=(%.1f, %.1f, %.1f) origin=(%.1f, %.1f, %.1f)' % (
    SPECIES, b.box_extent.x, b.box_extent.y, b.box_extent.z, b.origin.x, b.origin.y, b.origin.z))
unreal.log('QUIET_LAKE_TEXTURED_FISH_READY')
