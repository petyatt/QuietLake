"""Import the generated village props and build their extra materials (run as a UE Python commandlet).

Slots from GenerateVillageModels.py map to:
  M_Wood  -> /Game/Materials/M_WeatheredWood     M_Bark -> /Game/Materials/M_OldBark
  M_Paint -> M_VillagePaint (vertex colours, matte) M_Glass -> M_WindowGlass (dark, glossy)
  M_Net   -> M_Net (masked netting grid, two-sided)
"""
import os
import unreal

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = '/Game/Village'
PARTS = {'Boat_A': True, 'Boat_B': True, 'Izba': True, 'NetRack': False, 'Fence': False}   # name: collision


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def prop(src, out, p):
    if not lib.connect_material_property(src, out, p):
        raise RuntimeError('Cannot connect %s' % p)


def fresh(name):
    path = FOLDER + '/' + name
    if unreal.EditorAssetLibrary.does_asset_exist(path) and not unreal.EditorAssetLibrary.delete_asset(path):
        raise RuntimeError('Cannot delete ' + path)
    return assets.create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew()), path


paint, path = fresh('M_VillagePaint')
prop(node(paint, unreal.MaterialExpressionVertexColor), '', unreal.MaterialProperty.MP_BASE_COLOR)
prop(node(paint, unreal.MaterialExpressionConstant, r=0.78), '', unreal.MaterialProperty.MP_ROUGHNESS)
lib.recompile_material(paint)
unreal.EditorAssetLibrary.save_asset(path)

glass, path = fresh('M_WindowGlass')
prop(node(glass, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.01, 0.012, 0.012, 1)), '', unreal.MaterialProperty.MP_BASE_COLOR)
prop(node(glass, unreal.MaterialExpressionConstant, r=0.05), '', unreal.MaterialProperty.MP_ROUGHNESS)
prop(node(glass, unreal.MaterialExpressionConstant, r=1.0), '', unreal.MaterialProperty.MP_SPECULAR)
lib.recompile_material(glass)
unreal.EditorAssetLibrary.save_asset(path)

net, path = fresh('M_Net')
net.set_editor_property('two_sided', True)
net.set_editor_property('blend_mode', unreal.BlendMode.BLEND_MASKED)
uv = node(net, unreal.MaterialExpressionTextureCoordinate)
mask = node(net, unreal.MaterialExpressionCustom, code='float2 f = abs(frac(UV) - 0.5); return (max(f.x, f.y) > 0.43) ? 1.0 : 0.0;',
            output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT1)
item = unreal.CustomInput()
item.set_editor_property('input_name', 'UV')
mask.set_editor_property('inputs', [item])
if not lib.connect_material_expressions(uv, '', mask, 'UV'):
    raise RuntimeError('net link failed')
prop(mask, '', unreal.MaterialProperty.MP_OPACITY_MASK)
prop(node(net, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.32, 0.31, 0.27, 1)), '', unreal.MaterialProperty.MP_BASE_COLOR)
prop(node(net, unreal.MaterialExpressionConstant, r=0.9), '', unreal.MaterialProperty.MP_ROUGHNESS)
lib.recompile_material(net)
unreal.EditorAssetLibrary.save_asset(path)

slots = {
    'M_Wood': unreal.load_asset('/Game/Materials/M_WeatheredWood'),
    'M_Bark': unreal.load_asset('/Game/Materials/M_OldBark'),
    'M_Paint': paint, 'M_Glass': glass, 'M_Net': net,
}
order = ['M_Wood', 'M_Bark', 'M_Paint', 'M_Glass', 'M_Net']
for name, collision in PARTS.items():
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', os.path.join(ROOT, 'Tools', 'GeneratedVillage', 'SM_%s.fbx' % name))
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
    sm.set_editor_property('auto_generate_collision', collision)
    task.set_editor_property('options', options)
    assets.import_asset_tasks([task])
    mesh_path = FOLDER + '/SM_' + name
    mesh = unreal.load_asset(mesh_path)
    if not mesh:
        raise RuntimeError('Import failed: ' + name)
    materials = mesh.get_editor_property('static_materials')
    for i, m in enumerate(materials):
        slot = str(m.get_editor_property('material_slot_name'))
        key = slot if slot in slots else (order[i] if i < len(order) else 'M_Wood')
        mesh.set_material(i, slots[key])
        unreal.log('VILLAGE_SLOT %s %d %s -> %s' % (name, i, slot, key))
    unreal.EditorAssetLibrary.save_asset(mesh_path)
    b = mesh.get_bounds()
    unreal.log('VILLAGE_IMPORTED %s slots=%d extent=(%.0f, %.0f, %.0f)' % (name, len(materials), b.box_extent.x, b.box_extent.y, b.box_extent.z))
unreal.log('QUIET_LAKE_VILLAGE_READY')
