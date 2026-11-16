"""The engine Water surface with the game's live ripples: /Game/Water/M_VillageWater.

A copy of /Water/Materials/WaterSurface/Water_Material (engine assets stay read only) with one
addition at the very end of its attribute chain, where two things are added to the Water normal:
- the slope of the ALakeRipples height field (circles from the float, fish and rain); Ripples /
  RippleWindow are set at run time by ALakeRipples;
- the wind's fine ripple: the engine's tiling water normal map in three sizes, all drifting
  downwind. The engine's own fine ripple pans that map four ways at once and knows no wind, so the
  lake's instance turns it off (TuneVillageWater.py) and ULakeWaves drives this one: WindRipple
  (0 a mirror .. 1 a fresh wind) and RippleDrift (how far the two fine layers have drifted, cm;
  summed on the CPU so that a change of wind never makes the pattern jump).
With the defaults (black texture, zero window, no wind ripple) the water is the engine's. Then
TuneVillageWater.py parents the MI_Village* instances to it.

To rebuild the graph after changing this script, delete Content/Water/M_VillageWater.uasset first.

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
// The wind's ripple: fine and middling near the viewer, a broad one that carries to the far bank
// (the fine ones would only shimmer and show their tiling there).
if (Amount > 0.001)
{
    float nearby = 1 - smoothstep(1500, 6000, distance(P, C));
    float2 fine = Texture2DSample(Wind, WindSampler, (P.xy - Drift.xy) / 140.0).rg * 2 - 1;
    float2 mid = Texture2DSample(Wind, WindSampler, (P.xy - Drift.zw) / 430.0 + 0.37).rg * 2 - 1;
    float2 broad = Texture2DSample(Wind, WindSampler, (P.xy - Drift.zw * 1.6) / 2300.0 + 0.71).rg * 2 - 1;
    slope += (fine * 0.10 * nearby + mid * 0.14 * lerp(0.4, 1.0, nearby) + broad * 0.12) * Amount;
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

if 'WindRipple' in [str(n) for n in lib.get_scalar_parameter_names(mat)]:
    unreal.log('QUIET_LAKE_WATER_RIPPLES %s already has the ripple inputs' % path)
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
    camera = node(mat, unreal.MaterialExpressionCameraPositionWS)
    wind = node(mat, unreal.MaterialExpressionTextureObject,
                texture=unreal.load_asset('/Water/Textures/Normals/T_Water_TilingNormal_With_Height_02_Softened'))
    drift = node(mat, unreal.MaterialExpressionVectorParameter, parameter_name='RippleDrift',
                 default_value=unreal.LinearColor(0, 0, 0, 0))
    amount = node(mat, unreal.MaterialExpressionScalarParameter, parameter_name='WindRipple', default_value=0.0)
    custom = node(mat, unreal.MaterialExpressionCustom, code=RIPPLES, description='LakeRipples',
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    inputs = [('N', normal, normal_out), ('P', position, ''), ('Rip', ripple, ''), ('RW', window, 'RGBA'), ('Strength', strength, ''),
              ('C', camera, ''), ('Wind', wind, ''), ('Drift', drift, 'RGBA'), ('Amount', amount, '')]
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
    unreal.log('QUIET_LAKE_WATER_RIPPLES %s: live rings and wind ripple added to the Water normal (tangent space %s)' % (path, tangent))

assert unreal.EditorAssetLibrary.save_loaded_asset(mat), 'Cannot save ' + path
