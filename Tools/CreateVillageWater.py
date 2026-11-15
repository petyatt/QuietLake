"""The engine Water surface with the game's live ripples: /Game/Water/M_VillageWater.

A copy of /Water/Materials/WaterSurface/Water_Material (engine assets stay read only) with one
addition at the very end of its attribute chain: the slope of the ALakeRipples height field (circles
from the float, fish and rain) is added to the Water normal. Parameters Ripples / RippleWindow are
set at run time by ALakeRipples; with the defaults (black texture, zero window) the water is the
engine's. Then TuneVillageWater.py parents the MI_Village* instances to it.

Run with UnrealEditor-Cmd -run=pythonscript -script=C:/Fish/Tools/CreateVillageWater.py
"""
import unreal

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
SOURCE = '/Water/Materials/WaterSurface/Water_Material'
FOLDER, NAME = '/Game/Water', 'M_VillageWater'

# N: the Water normal in world space; RW = (origin x, origin y, size, one texel in UV) of the
# ripple window in cm; the height field fades out towards the window edge.
RIPPLES = """
float2 slope = -N.xy / max(N.z, 0.05);
if (RW.z > 0 && RW.w > 0)
{
    float2 uv = (P.xy - RW.xy) / RW.z;
    float edge = saturate(min(min(uv.x, uv.y), min(1 - uv.x, 1 - uv.y)) * 14);
    if (edge > 0)
    {
        float e = RW.w;
        float l = Texture2DSampleLevel(Rip, RipSampler, uv - float2(e, 0), 0).r;
        float r = Texture2DSampleLevel(Rip, RipSampler, uv + float2(e, 0), 0).r;
        float b = Texture2DSampleLevel(Rip, RipSampler, uv - float2(0, e), 0).r;
        float t = Texture2DSampleLevel(Rip, RipSampler, uv + float2(0, e), 0).r;
        slope += float2(r - l, t - b) / (2 * e * RW.z) * edge * Strength;
    }
}
return normalize(float3(-slope, 1));
"""


def node(mat, cls, **props):
    result = lib.create_material_expression(mat, cls)
    for key, value in props.items():
        result.set_editor_property(key, value)
    return result


def link(src, output, dst, input_name):
    if not lib.connect_material_expressions(src, output, dst, input_name):
        raise RuntimeError('Cannot connect %s.%s -> %s.%s' % (src.get_name(), output, dst.get_name(), input_name))


path = FOLDER + '/' + NAME
if unreal.EditorAssetLibrary.does_asset_exist(path):
    mat = unreal.load_asset(path)
else:
    mat = assets.duplicate_asset(NAME, FOLDER, unreal.load_asset(SOURCE))
assert isinstance(mat, unreal.Material), 'Cannot create ' + path

if 'Ripples' in [str(n) for n in lib.get_texture_parameter_names(mat)]:
    unreal.log('QUIET_LAKE_WATER_RIPPLES %s already has the ripple input' % path)
else:
    assert mat.get_editor_property('use_material_attributes'), 'Water must use material attributes'
    prop = unreal.MaterialProperty.MP_MATERIAL_ATTRIBUTES
    root = lib.get_material_property_input_node(mat, prop)
    root_output = lib.get_material_property_input_node_output_name(mat, prop)
    assert root, 'Water material has nothing in its attributes'

    split = node(mat, unreal.MaterialExpressionBreakMaterialAttributes)
    link(root, root_output, split, str(lib.get_material_expression_input_names(split)[0]))
    normal_output = next(str(n) for n in lib.get_material_expression_output_names(split) if str(n).lower() == 'normal')
    normal, normal_out = split, normal_output
    tangent = mat.get_editor_property('tangent_space_normal')
    if tangent:
        normal = node(mat, unreal.MaterialExpressionTransform)  # tangent -> world
        link(split, normal_output, normal, '')
        normal_out = ''

    ripple = node(mat, unreal.MaterialExpressionTextureObjectParameter, parameter_name='Ripples',
                  texture=unreal.load_asset('/Engine/EngineResources/Black'))
    window = node(mat, unreal.MaterialExpressionVectorParameter, parameter_name='RippleWindow',
                  default_value=unreal.LinearColor(0, 0, 0, 0))
    strength = node(mat, unreal.MaterialExpressionScalarParameter, parameter_name='LakeRippleStrength', default_value=1.0)
    position = node(mat, unreal.MaterialExpressionWorldPosition)
    custom = node(mat, unreal.MaterialExpressionCustom, code=RIPPLES, description='LakeRipples',
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    inputs = [('N', normal, normal_out), ('P', position, ''), ('Rip', ripple, ''), ('RW', window, 'RGBA'), ('Strength', strength, '')]
    pins = []
    for n, _, _ in inputs:
        pin = unreal.CustomInput()
        pin.set_editor_property('input_name', n)
        pins.append(pin)
    custom.set_editor_property('inputs', pins)
    for n, src, out in inputs:
        link(src, out, custom, n)
    result = custom
    if tangent:
        result = node(mat, unreal.MaterialExpressionTransform,
                      transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_WORLD,
                      transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_TANGENT)
        link(custom, '', result, '')

    joined = node(mat, unreal.MaterialExpressionSetMaterialAttributes)
    assert unreal.LakeLevelLibrary.add_set_attribute_inputs(joined, [unreal.MaterialProperty.MP_NORMAL.value])
    names = [str(n) for n in lib.get_material_expression_input_names(joined)]
    assert len(names) == 2, names
    link(root, root_output, joined, names[0])
    link(result, '', joined, names[1])
    assert lib.connect_material_property(joined, '', prop), 'Cannot connect the attributes output'
    lib.recompile_material(mat)
    unreal.log('QUIET_LAKE_WATER_RIPPLES %s: ripple slope added to the Water normal (tangent space %s)' % (path, tangent))

assert unreal.EditorAssetLibrary.save_loaded_asset(mat), 'Cannot save ' + path
