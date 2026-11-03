"""Import the prepared trees (Tools/PrepareTrees.py, Tools/PrepareTreeFoliage.py: a weeping willow, an
oak) as Nanite static meshes: the bark keeps the bought texture, the leaves are alpha-masked cards with a
two-sided foliage material swaying a little in the wind. Without the leafy version the plain one is used.

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/ImportTrees.py
"""
from pathlib import Path

import unreal

assets = unreal.AssetToolsHelpers.get_asset_tools()
lib = unreal.MaterialEditingLibrary
SOURCE = Path('C:/Fish/Tools/GeneratedTrees')
DIR = '/Game/Plants/Trees'
TEX_DIR = DIR + '/Textures'


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for key, value in props.items():
        n.set_editor_property(key, value)
    return n


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
    tex.set_editor_property('max_texture_size', 2048)
    unreal.EditorAssetLibrary.save_asset(tex.get_path_name())
    return tex


def leaf_material(name):
    """Masked two-sided foliage: the leaf texture, light through the leaves, a gentle sway."""
    tex = import_texture(SOURCE / ('T_%sLeaf.png' % name), 'T_%sLeaf' % name, True)
    tex.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_DEFAULT)
    unreal.EditorAssetLibrary.save_asset(tex.get_path_name())
    path = '%s/M_%sLeaves.M_%sLeaves' % (DIR, name, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    mat = assets.create_asset('M_%sLeaves' % name, DIR, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property('two_sided', True)
    mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_MASKED)
    mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    s = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name='Leaf', texture=tex)
    s.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    lib.connect_material_property(s, 'RGB', unreal.MaterialProperty.MP_BASE_COLOR)
    lib.connect_material_property(s, 'A', unreal.MaterialProperty.MP_OPACITY_MASK)
    sss = node(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.14, 0.19, 0.05, 1.0))
    lib.connect_material_property(sss, '', unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    r = node(mat, unreal.MaterialExpressionConstant, r=0.65)
    lib.connect_material_property(r, '', unreal.MaterialProperty.MP_ROUGHNESS)
    wind_fn = unreal.load_asset('/Engine/Functions/Engine_MaterialFunctions01/Wind/SimpleGrassWind.SimpleGrassWind')
    if wind_fn:
        wind = node(mat, unreal.MaterialExpressionMaterialFunctionCall)
        wind.set_editor_property('material_function', wind_fn)
        intensity = node(mat, unreal.MaterialExpressionConstant, r=0.08)
        weight = node(mat, unreal.MaterialExpressionConstant, r=1.0)
        speed = node(mat, unreal.MaterialExpressionConstant, r=0.6)
        lib.connect_material_expressions(intensity, '', wind, 'WindIntensity')
        lib.connect_material_expressions(weight, '', wind, 'WindWeight')
        lib.connect_material_expressions(speed, '', wind, 'WindSpeed')
        lib.connect_material_property(wind, '', unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path)
    return mat


for name in ('Willow', 'Oak'):
    leafy = SOURCE / ('SM_%sLeafy.fbx' % name)
    fbx = leafy if leafy.exists() else SOURCE / ('SM_%s_LOD0.fbx' % name)
    if not fbx.exists():
        unreal.log_warning('TREE missing ' + str(fbx))
        continue
    base = import_texture(SOURCE / ('T_%s_Base.png' % name), 'T_%s_Base' % name, True)
    normal_path = SOURCE / ('T_%s_Normal.png' % name)
    rough_path = SOURCE / ('T_%s_Rough.png' % name)
    normal = import_texture(normal_path, 'T_%s_Normal' % name, False, normal=True) if normal_path.exists() else None
    rough = import_texture(rough_path, 'T_%s_Rough' % name, False) if rough_path.exists() else None

    mat_path = '%s/M_%s.M_%s' % (DIR, name, name)
    if unreal.EditorAssetLibrary.does_asset_exist(mat_path):
        unreal.EditorAssetLibrary.delete_asset(mat_path)
    mat = assets.create_asset('M_' + name, DIR, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property('two_sided', True)
    bc = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name='BaseColour', texture=base)
    bc.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    lib.connect_material_property(bc, 'RGB', unreal.MaterialProperty.MP_BASE_COLOR)
    if normal:
        nrm = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name='NormalMap', texture=normal)
        nrm.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
        lib.connect_material_property(nrm, 'RGB', unreal.MaterialProperty.MP_NORMAL)
    if rough:
        rs = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name='Roughness', texture=rough)
        rs.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        lib.connect_material_property(rs, 'R', unreal.MaterialProperty.MP_ROUGHNESS)
    else:
        r = node(mat, unreal.MaterialExpressionConstant, r=0.8)
        lib.connect_material_property(r, '', unreal.MaterialProperty.MP_ROUGHNESS)
    spec = node(mat, unreal.MaterialExpressionConstant, r=0.2)
    lib.connect_material_property(spec, '', unreal.MaterialProperty.MP_SPECULAR)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(mat_path)

    mesh_path = '%s/SM_%s.SM_%s' % (DIR, name, name)
    if unreal.EditorAssetLibrary.does_asset_exist(mesh_path):
        unreal.EditorAssetLibrary.delete_asset(mesh_path)
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', str(fbx))
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
    sm.set_editor_property('build_nanite', True)
    task.set_editor_property('options', options)
    assets.import_asset_tasks([task])
    mesh = unreal.load_asset(mesh_path)
    if not mesh:
        raise RuntimeError('mesh import failed: ' + mesh_path)
    mesh.set_material(0, mat)
    if leafy.exists() and mesh.get_num_sections(0) > 1:
        mesh.set_material(1, leaf_material(name))
    unreal.EditorAssetLibrary.save_asset(mesh_path)
    b = mesh.get_bounds()
    unreal.log('TREE_IMPORTED %s%s sections=%d extent=(%.0f, %.0f, %.0f) cm' % (name, ' (leafy)' if leafy.exists() else '', mesh.get_num_sections(0), b.box_extent.x, b.box_extent.y, b.box_extent.z))

unreal.log('QUIET_LAKE_TREES_READY')
