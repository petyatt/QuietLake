"""Import the output of PrepareVillageAssets.py with Unreal's -ExecutePythonScript.

QUIETLAKE_VILLAGE_ONLY optionally selects comma-separated prepared assets.
QUIETLAKE_VILLAGE_TEXTURES_ONLY=1 updates only the existing texture packages.
"""
import os
import struct
from pathlib import Path

import unreal


root = Path(__file__).resolve().parents[1] / 'Saved' / 'VillageAssets'
folder = '/Game/Village/Lakeside'
assets = unreal.AssetToolsHelpers.get_asset_tools()
editor = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
materials = unreal.MaterialEditingLibrary
texture_folder = folder + '/Textures'


def is_building(name):
    return name.startswith('House') or name in {'Shop', 'Cabin', 'WoodShed', 'LowShed', 'PlankShed', 'LeanTo'}


def import_asset(source, name, path, options=None):
    task = unreal.AssetImportTask()
    for key, value in dict(filename=str(source), destination_name=name, destination_path=path,
                           automated=True, replace_existing=True, replace_existing_settings=True, save=False).items():
        task.set_editor_property(key, value)
    if options:
        task.set_editor_property('options', options)
    assets.import_asset_tasks([task])
    asset = unreal.load_asset(path + '/' + name)
    if not asset:
        raise RuntimeError('Import failed: ' + str(source))
    return asset


def import_surface_texture(name, kind, source):
    tex = import_asset(source, f'T_{name}_{kind}', texture_folder)
    tex.set_editor_property('srgb', kind == 'Base')
    if kind == 'Base' and is_building(name):
        tex.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_BC7)
    elif kind == 'Normal':
        tex.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_NORMALMAP)
        tex.set_editor_property('flip_green_channel', True)
    elif kind == 'Rough':
        tex.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_GRAYSCALE)
    if not unreal.EditorAssetLibrary.save_loaded_asset(tex):
        raise RuntimeError(f'{name}: could not save {kind} texture')
    return tex


def refresh_textures(name, source):
    for kind in ('Base', 'Normal', 'Rough'):
        path = source / (kind + '.png')
        if not unreal.EditorAssetLibrary.does_asset_exist(f'{texture_folder}/T_{name}_{kind}'):
            raise RuntimeError(f'{name}: missing the existing {kind} texture')
        with path.open('rb') as png:
            header = png.read(24)
        if header[:8] != b'\x89PNG\r\n\x1a\n' or header[12:16] != b'IHDR':
            raise RuntimeError(f'{path}: expected a PNG image')
        expected = struct.unpack('>II', header[16:24])
        tex = import_surface_texture(name, kind, path)
        actual = (tex.blueprint_get_size_x(), tex.blueprint_get_size_y())
        if actual != expected:
            raise RuntimeError(f'{name} {kind}: expected {expected}, got {actual}')
        unreal.log(f'VILLAGE_TEXTURE {name} {kind}: {actual[0]}x{actual[1]} {tex.get_editor_property("compression_settings")}')


def make_material(name, source):
    path = folder + '/M_' + name
    mat = unreal.EditorAssetLibrary.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if mat:
        materials.delete_all_material_expressions(mat)
    else:
        mat = assets.create_asset('M_' + name, folder, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property('used_with_instanced_static_meshes', True)
    mat.set_editor_property('two_sided', name in {'Reeds', 'Lilies', 'FlowerBush', 'Clothesline'})
    for kind, output, prop, sampler in (
        ('Base', 'RGB', unreal.MaterialProperty.MP_BASE_COLOR, unreal.MaterialSamplerType.SAMPLERTYPE_COLOR),
        ('Normal', 'RGB', unreal.MaterialProperty.MP_NORMAL, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL),
        ('Rough', 'R', unreal.MaterialProperty.MP_ROUGHNESS, unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE),
    ):
        tex = import_surface_texture(name, kind, source / (kind + '.png'))
        node = materials.create_material_expression(mat, unreal.MaterialExpressionTextureSample)
        node.set_editor_property('texture', tex)
        node.set_editor_property('sampler_type', sampler)
        if not materials.connect_material_property(node, output, prop):
            raise RuntimeError(f'{name}: could not connect {kind}')
    materials.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    return mat


def import_mesh(name, source):
    options = unreal.FbxImportUI()
    for key in ('import_materials', 'import_textures', 'import_as_skeletal'):
        options.set_editor_property(key, False)
    settings = options.get_editor_property('static_mesh_import_data')
    for key in ('auto_generate_collision', 'generate_lightmap_u_vs', 'build_nanite'):
        settings.set_editor_property(key, False)
    settings.set_editor_property('combine_meshes', True)
    building = is_building(name)
    if building:
        settings.set_editor_property('normal_import_method', unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS)
    mesh = import_asset(source / f'SM_{name}.fbx', 'SM_' + name, folder, options)
    surface = (unreal.load_asset(folder + '/M_Vegetables') if name == 'GardenCrops'
               else make_material(name, source))
    if not surface:
        raise RuntimeError(f'{name}: import Vegetables first')
    for index, _ in enumerate(mesh.get_editor_property('static_materials')):
        mesh.set_material(index, surface)
    reduction = unreal.StaticMeshReductionOptions()
    reduction.set_editor_property('auto_compute_lod_screen_size', False)
    reduction.set_editor_property('reduction_settings', [
        unreal.StaticMeshReductionSettings(percent_triangles=fraction, screen_size=screen)
        for fraction, screen in ([(1.0, 1.0), (0.28, 0.16), (0.065, 0.055)] if building
                                 else [(1.0, 1.0), (0.28, 0.20), (0.07, 0.065)])
    ])
    if editor.set_lods(mesh, reduction) != 3:
        raise RuntimeError(f'{name}: expected three LODs')
    if building:
        body = mesh.get_editor_property('body_setup')
        body.set_editor_property('collision_trace_flag', unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
    elif name in {'Fence', 'RailFence', 'Gate', 'Boulders', 'FlatRock'}:
        editor.add_simple_collisions(mesh, unreal.ScriptCollisionShapeType.BOX)
    if not unreal.EditorAssetLibrary.save_loaded_asset(mesh):
        raise RuntimeError(f'{name}: could not save mesh')
    bounds = mesh.get_bounds()
    unreal.log(f'VILLAGE_ASSET {name}: 3 LODs, bounds {bounds.origin} +/- {bounds.box_extent}')


def main():
    if not editor:
        raise RuntimeError('Run this script in the full editor, with -ExecutePythonScript')
    only = set(filter(None, os.environ.get('QUIETLAKE_VILLAGE_ONLY', '').split(',')))
    textures_only = os.environ.get('QUIETLAKE_VILLAGE_TEXTURES_ONLY') == '1'
    sources = {path.name: path for path in root.iterdir()
               if (path / ('Base.png' if textures_only else 'SM_' + path.name + '.fbx')).is_file()}
    if not sources or only - sources.keys():
        raise RuntimeError(f'Missing prepared assets: {only - sources.keys()}')
    for name in sorted(only or sources, key=lambda name: (name == 'GardenCrops', name)):
        (refresh_textures if textures_only else import_mesh)(name, sources[name])
    result = 'TEXTURES' if textures_only else 'ASSETS'
    unreal.log(f'QUIET_LAKE_LAKESIDE_{result}_READY {len(only or sources)}')


try:
    main()
finally:
    unreal.SystemLibrary.quit_editor()
