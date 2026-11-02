"""Import the prepared pebbles into the editor: textures, a material each, and a static mesh with
three LODs. Run in the full editor (the commandlet has no static mesh subsystem):

    UnrealEditor.exe C:/Fish/QuietLake.uproject -ExecutePythonScript="C:/Fish/Tools/ImportPebblesRun.py"
"""
import os
from pathlib import Path

import unreal

assets = unreal.AssetToolsHelpers.get_asset_tools()
sub = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
lib = unreal.MaterialEditingLibrary

SOURCE = Path(os.environ.get('QUIETLAKE_PEBBLE_ASSETS', 'C:/Fish/Saved/PebbleAssets'))
MESHES = Path('C:/Fish/Tools/GeneratedBottom/Meshes')
DIR = '/Game/Bottom'
TEX_DIR = DIR + '/Textures'

PEBBLES = ['Pebble_Asymmetric', 'Pebble_FlatOval', 'Pebble_Round', 'Pebble_Teardrop']


def node(mat, cls, **kwargs):
    n = lib.create_material_expression(mat, cls)
    for key, value in kwargs.items():
        n.set_editor_property(key, value)
    return n


def connect(a, out, b, inp):
    lib.connect_material_expressions(a, out, b, inp)


def import_texture(path, name, srgb, normal=False):
    full = '%s/%s' % (TEX_DIR, name)
    if unreal.EditorAssetLibrary.does_asset_exist(full):
        unreal.EditorAssetLibrary.delete_asset(full)
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', str(path))
    task.set_editor_property('destination_path', TEX_DIR)
    task.set_editor_property('destination_name', name)
    task.set_editor_property('automated', True)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('save', True)
    assets.import_asset_tasks([task])
    tex = unreal.load_asset(full)
    if not tex:
        raise RuntimeError('texture import failed: ' + str(path))
    tex.set_editor_property('srgb', srgb)
    if normal:
        tex.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_NORMALMAP)
        tex.set_editor_property('flip_green_channel', True)
    tex.set_editor_property('lod_group', unreal.TextureGroup.TEXTUREGROUP_WORLD)
    unreal.EditorAssetLibrary.save_asset(tex.get_path_name())
    return tex


for name in PEBBLES:
    tex_base = import_texture(SOURCE / 'Textures' / (name + '_Base.png'), 'T_' + name + '_Base', True)
    tex_normal = import_texture(SOURCE / 'Textures' / (name + '_Normal.png'), 'T_' + name + '_Normal', False, normal=True)
    tex_rough = import_texture(SOURCE / 'Textures' / (name + '_Rough.png'), 'T_' + name + '_Rough', False)

    mat_path = '%s/M_%s.M_%s' % (DIR, name, name)
    if unreal.EditorAssetLibrary.does_asset_exist(mat_path):
        unreal.EditorAssetLibrary.delete_asset(mat_path)
    mat = assets.create_asset('M_' + name, DIR, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property('two_sided', False)
    base = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name='BaseColour', texture=tex_base)
    base.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    # Wet stones in the water are darker than the dry scan, with a brown-green algae cast.
    dark = node(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.44, 0.41, 0.33, 1.0))
    mul = node(mat, unreal.MaterialExpressionMultiply)
    connect(base, 'RGB', mul, 'A')
    connect(dark, '', mul, 'B')
    lib.connect_material_property(mul, '', unreal.MaterialProperty.MP_BASE_COLOR)
    normal = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name='NormalMap', texture=tex_normal)
    normal.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    lib.connect_material_property(normal, 'RGB', unreal.MaterialProperty.MP_NORMAL)
    rough = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name='Roughness', texture=tex_rough)
    rough.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    rough_add = node(mat, unreal.MaterialExpressionAdd, const_a=0.15)
    connect(rough, 'R', rough_add, 'A')
    lib.connect_material_property(rough_add, '', unreal.MaterialProperty.MP_ROUGHNESS)
    spec = node(mat, unreal.MaterialExpressionConstant, r=0.22)
    lib.connect_material_property(spec, '', unreal.MaterialProperty.MP_SPECULAR)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(mat_path)

    mesh_path = '%s/SM_%s.SM_%s' % (DIR, name, name)
    if unreal.EditorAssetLibrary.does_asset_exist(mesh_path):
        unreal.EditorAssetLibrary.delete_asset(mesh_path)
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', str(MESHES / ('SM_%s_LOD0.fbx' % name)))
    task.set_editor_property('destination_path', DIR)
    task.set_editor_property('destination_name', 'SM_' + name)
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
    sm.set_editor_property('auto_generate_collision', False)
    task.set_editor_property('options', options)
    assets.import_asset_tasks([task])
    mesh = unreal.load_asset(mesh_path)
    if not mesh:
        raise RuntimeError('mesh import failed: ' + mesh_path)
    mesh.set_material(0, mat)
    for lod in (1, 2):
        src = MESHES / ('SM_%s_LOD%d.fbx' % (name, lod))
        if sub.import_lod(mesh, lod, str(src)) != lod:
            raise RuntimeError('LOD %d import failed for %s' % (lod, name))
    sub.set_lod_screen_sizes(mesh, [1.0, 0.3, 0.08])
    unreal.EditorAssetLibrary.save_asset(mesh_path)
    b = mesh.get_bounds()
    unreal.log('PEBBLE_MESH %s lods=%d extent=(%.1f, %.1f, %.1f) cm' % (
        name, sub.get_lod_count(mesh), b.box_extent.x, b.box_extent.y, b.box_extent.z))

print('PEBBLE_IMPORT_READY')
