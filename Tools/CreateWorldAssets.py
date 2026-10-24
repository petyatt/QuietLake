"""Create original procedural materials and import our synthesized ambience."""
import os
import unreal

assets = unreal.AssetToolsHelpers.get_asset_tools()
lib = unreal.MaterialEditingLibrary

def expression(material, cls, **props):
    node = lib.create_material_expression(material, cls)
    for key, value in props.items():
        node.set_editor_property(key, value)
    return node

def connect(source, output, target, input_name):
    # The engine silently ignores bad pin names, which once left the terrain black.
    if not lib.connect_material_expressions(source, output, target, input_name):
        raise RuntimeError('Material link failed: %s.%s -> %s.%s' % (source.get_name(), output, target.get_name(), input_name))

def custom(material, code, inputs, output=unreal.CustomMaterialOutputType.CMOT_FLOAT3):
    node = expression(material, unreal.MaterialExpressionCustom, code=code, output_type=output)
    entries = []
    for name, source in inputs:
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        entries.append(item)
    node.set_editor_property('inputs', entries)
    for name, source in inputs:
        connect(source, '', node, name)
    return node

palette = {
    'M_Terrain': (0.15,0.24,0.08), 'M_Bark': (0.12,0.058,0.024),
    'M_Birch': (0.7,0.7,0.59), 'M_Pine': (0.035,0.13,0.065),
    'M_Leaves': (0.16,0.30,0.06), 'M_Stone': (0.25,0.28,0.25),
    'M_Grass': (0.20,0.28,0.055), 'M_Reeds': (0.28,0.27,0.065),
    'M_Planks': (0.26,0.14,0.064), 'M_Metal': (0.18,0.24,0.25),
    'M_LakeSurface': (0.018,0.13,0.15),
}
# Hash-based value noise, 5 octaves; shared by terrain colour and roughness.
NOISE = """
float2 p = P.xy * 0.0035; float n = 0; float a = 0.5;
for (int i = 0; i < 5; i++) {
    float2 c = floor(p); float2 f = frac(p); f = f * f * (3 - 2 * f);
    float h00 = frac(sin(dot(c, float2(127.1, 311.7))) * 43758.5453);
    float h10 = frac(sin(dot(c + float2(1, 0), float2(127.1, 311.7))) * 43758.5453);
    float h01 = frac(sin(dot(c + float2(0, 1), float2(127.1, 311.7))) * 43758.5453);
    float h11 = frac(sin(dot(c + float2(1, 1), float2(127.1, 311.7))) * 43758.5453);
    n += a * lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
    p = p * 2.07 + 17.3; a *= 0.5;
}
float wet = 1 - saturate((P.z + 22) / 7);   // last ~7 cm above the waterline at Z=-25
"""

# Six directional waves (deep-water dispersion, slowed for a calm lake) plus distance fade.
WAVES = """
float2 dirs[6] = { float2(1,0.2), float2(0.7,0.7), float2(-0.3,1), float2(0.9,-0.45), float2(-0.8,0.55), float2(0.15,-1) };
float lens[6] = { 340, 190, 120, 75, 41, 23 };
float steep[6] = { 0.018, 0.02, 0.022, 0.024, 0.02, 0.016 };  // calm morning surface
float2 g = 0;
for (int i = 0; i < 6; i++) {
    float2 d = normalize(dirs[i]);
    float k = 6.2831853 / lens[i];
    float w = sqrt(980.0 * k) * 0.55;
    g += d * steep[i] * cos(dot(d, P.xy) * k - T * w);
}
float fade = lerp(1.0, 0.15, saturate(distance(P, C) / 4000));
return normalize(float3(-g * fade, 1));
"""

for name, rgb in palette.items():
    path = '/Game/Materials/' + name
    # Recreate from scratch: delete_all_material_expressions keeps special output nodes
    # (e.g. Single Layer Water), so re-running the script used to stack duplicates.
    if unreal.EditorAssetLibrary.does_asset_exist(path) and not unreal.EditorAssetLibrary.delete_asset(path):
        raise RuntimeError('Cannot delete old material: ' + path)
    mat = assets.create_asset(name, '/Game/Materials', unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property('used_with_instanced_static_meshes', True)
    mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    position = expression(mat, unreal.MaterialExpressionWorldPosition)
    time = expression(mat, unreal.MaterialExpressionTime)

    if name == 'M_LakeSurface':
        # UE5 Single Layer Water: depth absorption, scattering, Lumen reflections.
        mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER)
        camera = expression(mat, unreal.MaterialExpressionCameraPositionWS)
        normal = custom(mat, WAVES, [('P', position), ('T', time), ('C', camera)])
        lib.connect_material_property(normal, '', unreal.MaterialProperty.MP_NORMAL)
        base = expression(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.01, 0.013, 0.012, 1))
        lib.connect_material_property(base, '', unreal.MaterialProperty.MP_BASE_COLOR)
        rough = expression(mat, unreal.MaterialExpressionConstant, r=0.035)
        lib.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)
        out = expression(mat, unreal.MaterialExpressionSingleLayerWaterMaterialOutput)
        # Peaty forest lake: red absorbed fast, greenish scattering (coefficients per metre).
        scatter = expression(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.010, 0.022, 0.016, 1))
        absorb = expression(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.55, 0.20, 0.26, 1))
        connect(scatter, '', out, 'ScatteringCoefficients')
        connect(absorb, '', out, 'AbsorptionCoefficients')
    elif name == 'M_Terrain':
        vertex = expression(mat, unreal.MaterialExpressionVertexColor)
        color = custom(mat, NOISE + """
float3 c = V.rgb * lerp(0.72, 1.18, n);
c = lerp(c, c * float3(0.55, 0.52, 0.5), wet);   // damp ground is darker
return c;""", [('P', position), ('V', vertex)])
        lib.connect_material_property(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
        rough = custom(mat, NOISE + "return lerp(lerp(0.82, 0.95, n), 0.35, wet);", [('P', position)],
                       unreal.CustomMaterialOutputType.CMOT_FLOAT1)
        lib.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)
    else:
        color = expression(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(*rgb, 1))
        detail = '0.85+0.15*sin(P.x*0.055+sin(P.y*0.033))*sin(P.z*0.07+P.y*0.05)'
        if name == 'M_Birch':
            detail = '0.22+0.78*smoothstep(0.10,0.23,abs(sin(P.z*0.12+sin(P.y*0.13))))'
        elif name in ('M_Planks', 'M_Bark'):
            detail = '0.7+0.3*abs(sin(P.y*0.15+sin(P.z*0.04+P.x*0.025)))'
        color = custom(mat, 'return C*(' + detail + ');', [('P', position), ('C', color)])
        lib.connect_material_property(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
        rough = expression(mat, unreal.MaterialExpressionConstant, r=0.82)
        lib.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)
    if name in ('M_Pine', 'M_Leaves', 'M_Grass', 'M_Reeds'):
        sway = custom(mat, 'return float3(sin(T*1.3+P.x*0.003+P.y*0.002)*2,cos(T+P.y*0.004)*1.3,0);', [('P', position), ('T', time)])
        lib.connect_material_property(sway, '', unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path)

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for name in ('A_Wind','A_Water','A_Birds'):
    task = unreal.AssetImportTask()
    task.set_editor_property('filename',os.path.join(root,'Tools','GeneratedAudio',name+'.wav'))
    task.set_editor_property('destination_path','/Game/Audio')
    task.set_editor_property('automated',True)
    task.set_editor_property('replace_existing',True)
    task.set_editor_property('save',True)
    assets.import_asset_tasks([task])
    sound = unreal.load_asset('/Game/Audio/'+name)
    if not sound:
        raise RuntimeError('Audio import failed: '+name)
    sound.set_editor_property('looping',True)
    unreal.EditorAssetLibrary.save_asset('/Game/Audio/'+name)
unreal.log('QUIET_LAKE_WORLD_ASSETS_READY')
