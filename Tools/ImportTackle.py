"""Import the generated tackle and build its materials (run as a UE Python commandlet).

M_Tackle       - vertex colours; vertex alpha 0 = metal (hook, eyes), 0.5 = fluorescent paint (glows), 1 = plain.
M_RodBlank     - dark glossy carbon for the rod sections (engine cylinder).
M_FishingLine  - pale green monofilament for the cable components.
M_TackleCloth  - vertex colours, matt canvas (the rod case).
M_Ghost        - translucent unlit tint (parameter Tint) for placement ghosts.

QUIETLAKE_TACKLE=RodCase,RodCaseLid imports only those parts and keeps the existing materials
(M_Tackle is tuned afterwards by TuneTackleHighlights.py); missing M_TackleCloth / M_Ghost are created.
"""
import os
import unreal

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PARTS = ['Bobber', 'Hook', 'Worm', 'Maggot', 'Bread', 'Sinker', 'Reel', 'RodHandle', 'RodStand',
         'RodCase', 'RodCaseLid', 'RodCaseStrap', 'RodPacked_Light', 'RodPacked_Medium', 'RodPacked_Heavy']
CLOTH = {'RodCase', 'RodCaseLid', 'RodCaseStrap'}
ONLY = [p for p in os.environ.get('QUIETLAKE_TACKLE', '').split(',') if p]


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def connect(src, out, dst, inp):
    if not lib.connect_material_expressions(src, out, dst, inp):
        raise RuntimeError('Material link failed: %s.%s -> %s.%s' % (src.get_name(), out, dst.get_name(), inp))


def prop(src, out, p):
    if not lib.connect_material_property(src, out, p):
        raise RuntimeError('Cannot connect %s' % p)


def fresh(path, name):
    if unreal.EditorAssetLibrary.does_asset_exist(path) and not unreal.EditorAssetLibrary.delete_asset(path):
        raise RuntimeError('Cannot delete ' + path)
    return assets.create_asset(name, '/Game/Tackle', unreal.Material, unreal.MaterialFactoryNew())


def build_base_materials():
    global mat
    # M_Tackle
    mat = fresh('/Game/Tackle/M_Tackle', 'M_Tackle')
    mat.set_editor_property('two_sided', True)
    vc = node(mat, unreal.MaterialExpressionVertexColor)
    prop(vc, '', unreal.MaterialProperty.MP_BASE_COLOR)


    def custom(code, output):
        """Custom node with V = vertex RGB and A = vertex alpha (the RGB pin carries no alpha)."""
        n = node(mat, unreal.MaterialExpressionCustom, code=code, output_type=output)
        items = []
        for name in ('V', 'A'):
            item = unreal.CustomInput()
            item.set_editor_property('input_name', name)
            items.append(item)
        n.set_editor_property('inputs', items)
        connect(vc, '', n, 'V')
        connect(vc, 'A', n, 'A')
        return n


    F1, F3 = unreal.CustomMaterialOutputType.CMOT_FLOAT1, unreal.CustomMaterialOutputType.CMOT_FLOAT3
    prop(custom('return saturate(1 - A * 4);', F1), '', unreal.MaterialProperty.MP_METALLIC)
    prop(custom('return lerp(0.22, 0.5, saturate(A * 4));', F1), '', unreal.MaterialProperty.MP_ROUGHNESS)
    # Fluorescent paint (alpha 0.5, or saturated red/orange) glows a little so the float reads from the bank.
    prop(custom('float paint = max(saturate(1 - abs(A - 0.5) * 8), (V.r > 0.7 && V.g < 0.45 && V.b < 0.12) ? 1.0 : 0.0); return V * paint * 1.5;', F3),
         '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset('/Game/Tackle/M_Tackle')

    # M_RodBlank
    mat = fresh('/Game/Tackle/M_RodBlank', 'M_RodBlank')
    prop(node(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.06, 0.07, 0.06, 1)), '', unreal.MaterialProperty.MP_BASE_COLOR)
    prop(node(mat, unreal.MaterialExpressionConstant, r=0.22), '', unreal.MaterialProperty.MP_ROUGHNESS)
    prop(node(mat, unreal.MaterialExpressionConstant, r=0.3), '', unreal.MaterialProperty.MP_METALLIC)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset('/Game/Tackle/M_RodBlank')

    # M_FishingLine
    mat = fresh('/Game/Tackle/M_FishingLine', 'M_FishingLine')
    mat.set_editor_property('two_sided', True)
    # Light, slightly glinting so the thin line reads against water and fog (real mono glints in the sun).
    prop(node(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.85, 0.88, 0.82, 1)), '', unreal.MaterialProperty.MP_BASE_COLOR)
    # The glint fades at night (MPC_Weather.Daylight), so the line does not glow in the dark.
    glint = node(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.12, 0.12, 0.11, 1))
    day = node(mat, unreal.MaterialExpressionCollectionParameter, collection=unreal.load_asset('/Game/Weather/MPC_Weather'), parameter_name='Daylight')
    glow = node(mat, unreal.MaterialExpressionMultiply)
    connect(glint, '', glow, 'A')
    connect(day, '', glow, 'B')
    prop(glow, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    prop(node(mat, unreal.MaterialExpressionConstant, r=0.2), '', unreal.MaterialProperty.MP_ROUGHNESS)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset('/Game/Tackle/M_FishingLine')



def build_cloth_material():
    m = fresh('/Game/Tackle/M_TackleCloth', 'M_TackleCloth')
    m.set_editor_property('two_sided', True)
    v = lib.create_material_expression(m, unreal.MaterialExpressionVertexColor)
    prop(v, '', unreal.MaterialProperty.MP_BASE_COLOR)
    r = lib.create_material_expression(m, unreal.MaterialExpressionConstant)
    r.set_editor_property('r', 0.9)
    prop(r, '', unreal.MaterialProperty.MP_ROUGHNESS)
    lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_asset('/Game/Tackle/M_TackleCloth')


def build_ghost_material():
    m = fresh('/Game/Tackle/M_Ghost', 'M_Ghost')
    m.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property('two_sided', True)
    tint = lib.create_material_expression(m, unreal.MaterialExpressionVectorParameter)
    tint.set_editor_property('parameter_name', 'Tint')
    tint.set_editor_property('default_value', unreal.LinearColor(0.25, 0.55, 1.0, 1))
    glow = lib.create_material_expression(m, unreal.MaterialExpressionMultiply)
    connect(tint, '', glow, 'A')
    two = lib.create_material_expression(m, unreal.MaterialExpressionConstant)
    two.set_editor_property('r', 1.6)
    connect(two, '', glow, 'B')
    prop(glow, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    # Brighter at the edges, so the outline reads over grass and water.
    fres = lib.create_material_expression(m, unreal.MaterialExpressionFresnel)
    edge = lib.create_material_expression(m, unreal.MaterialExpressionMultiply)
    connect(fres, '', edge, 'A')
    k = lib.create_material_expression(m, unreal.MaterialExpressionConstant)
    k.set_editor_property('r', 0.45)
    connect(k, '', edge, 'B')
    add = lib.create_material_expression(m, unreal.MaterialExpressionAdd)
    base = lib.create_material_expression(m, unreal.MaterialExpressionConstant)
    base.set_editor_property('r', 0.3)
    connect(base, '', add, 'A')
    connect(edge, '', add, 'B')
    prop(add, '', unreal.MaterialProperty.MP_OPACITY)
    lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_asset('/Game/Tackle/M_Ghost')


if not ONLY:
    build_base_materials()
if not ONLY or not unreal.EditorAssetLibrary.does_asset_exist('/Game/Tackle/M_TackleCloth'):
    build_cloth_material()
if not ONLY or not unreal.EditorAssetLibrary.does_asset_exist('/Game/Tackle/M_Ghost'):
    build_ghost_material()

tackle = unreal.load_asset('/Game/Tackle/M_Tackle')
cloth = unreal.load_asset('/Game/Tackle/M_TackleCloth')
for name in PARTS:
    if ONLY and name not in ONLY:
        continue
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', os.path.join(ROOT, 'Tools', 'GeneratedTackle', 'SM_%s.fbx' % name))
    task.set_editor_property('destination_path', '/Game/Tackle')
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
    path = '/Game/Tackle/SM_' + name
    mesh = unreal.load_asset(path)
    if not mesh:
        raise RuntimeError('Import failed: ' + name)
    mesh.set_material(0, cloth if name in CLOTH else tackle)
    unreal.EditorAssetLibrary.save_asset(path)
    b = mesh.get_bounds()
    unreal.log('TACKLE_IMPORTED %s extent=(%.2f, %.2f, %.2f) origin=(%.2f, %.2f, %.2f)' % (
        name, b.box_extent.x, b.box_extent.y, b.box_extent.z, b.origin.x, b.origin.y, b.origin.z))
unreal.log('QUIET_LAKE_TACKLE_READY')
