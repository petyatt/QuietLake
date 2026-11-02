"""Import the prepared bottom props (log, branches, shell, crayfish) with three LODs and materials.

    UnrealEditor.exe C:/Fish/QuietLake.uproject -ExecutePythonScript="C:/Fish/Tools/ImportBottomPropsRun.py"
"""
import os
from pathlib import Path

import unreal

assets = unreal.AssetToolsHelpers.get_asset_tools()
sub = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
lib = unreal.MaterialEditingLibrary

SOURCE = Path(os.environ.get('QUIETLAKE_BOTTOM_ASSETS', 'C:/Fish/Saved/BottomAssets'))
MESHES = Path('C:/Fish/Tools/GeneratedBottom/Props')
DIR = '/Game/Bottom'
TEX_DIR = DIR + '/Textures'

PROPS = [
    # mesh name, texture name, tint (wet, algae)
    ('BottomLog', 'Bottom_Log', (0.60, 0.56, 0.46)),
    ('BottomBranch1', 'Bottom_Branch1', (0.58, 0.54, 0.44)),
    ('BottomBranch2', 'Bottom_Branch2', (0.58, 0.54, 0.44)),
    ('BottomBranch3', 'Bottom_Branch3', (0.58, 0.54, 0.44)),
    ('BottomShell', 'Bottom_Shell', (0.78, 0.74, 0.66)),
    ('BottomCrayfish', 'Bottom_Crayfish', (0.85, 0.80, 0.72)),
]


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for key, value in props.items():
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


for name, tex, tint in PROPS:
    base = import_texture(SOURCE / 'Textures' / (tex + '_Base.png'), 'T_' + name + '_Base', True)
    normal_path = SOURCE / 'Textures' / (tex + '_Normal.png')
    rough_path = SOURCE / 'Textures' / (tex + '_Rough.png')
    normal = import_texture(normal_path, 'T_' + name + '_Normal', False, normal=True) if normal_path.exists() else None
    rough = import_texture(rough_path, 'T_' + name + '_Rough', False) if rough_path.exists() else None

    mat_path = '%s/M_%s.M_%s' % (DIR, name, name)
    if unreal.EditorAssetLibrary.does_asset_exist(mat_path):
        unreal.EditorAssetLibrary.delete_asset(mat_path)
    mat = assets.create_asset('M_' + name, DIR, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property('two_sided', False)
    bc = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name='BaseColour', texture=base)
    bc.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    wet = node(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(tint[0], tint[1], tint[2], 1.0))
    mul = node(mat, unreal.MaterialExpressionMultiply)
    connect(bc, 'RGB', mul, 'A')
    connect(wet, '', mul, 'B')
    lib.connect_material_property(mul, '', unreal.MaterialProperty.MP_BASE_COLOR)
    if normal:
        nrm = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name='NormalMap', texture=normal)
        nrm.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
        lib.connect_material_property(nrm, 'RGB', unreal.MaterialProperty.MP_NORMAL)
    r = node(mat, unreal.MaterialExpressionConstant, r=0.6 if rough is None else 0.4)
    if rough:
        rs = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name='Roughness', texture=rough)
        rs.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        connect(rs, 'R', r, '')
    lib.connect_material_property(r, '', unreal.MaterialProperty.MP_ROUGHNESS)
    spec = node(mat, unreal.MaterialExpressionConstant, r=0.15)
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
    lods = 1
    for lod in (1, 2):
        src = MESHES / ('SM_%s_LOD%d.fbx' % (name, lod))
        if not src.exists():
            break
        if sub.import_lod(mesh, lod, str(src)) == lod:
            lods += 1
    if lods == 3:
        sub.set_lod_screen_sizes(mesh, [1.0, 0.3, 0.08])
    unreal.EditorAssetLibrary.save_asset(mesh_path)
    b = mesh.get_bounds()
    unreal.log('BOTTOM_PROP %s lods=%d extent=(%.1f, %.1f, %.1f) cm' % (
        name, sub.get_lod_count(mesh), b.box_extent.x, b.box_extent.y, b.box_extent.z))

print('BOTTOM_PROPS_IMPORT_READY')
