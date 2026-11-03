"""Build M_TerrainScanned: the lake terrain material from Quixel Megascans surfaces.

Layers (world-aligned, no UV seams on the procedural terrain):
  meadow -> Uncut Grass, forest and shore -> Mossy Forest Floor, patches -> Nordic Moss,
  plus darker, glossier damp ground right above the waterline.
Terrain vertex colour carries the layer masks: R = meadow, G = forest, B = bare clay (set in LakeEnvironment.cpp).
"""
import unreal

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
PATH = '/Game/Materials/M_TerrainScanned'
SURF = '/Game/Fab/Megascans/Surfaces/'
LAYERS = {
    # name: (texture folder, texture id, tile size in cm)
    'G': (SURF + 'Uncut_Grass_oilpt20/High/oilpt20_tier_1/Textures/', 'oilpt20', 220.0),
    'F': (SURF + 'Mossy_Forest_Floor_vfylbge/High/vfylbge_tier_1/Textures/', 'vfylbge', 160.0),
    'M': (SURF + 'Nordic_Moss_se4rwei/High/se4rwei_tier_1/Textures/', 'se4rwei', 200.0),
}


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def connect(src, out, dst, inp):
    # The engine silently ignores wrong pin names, so fail loudly instead.
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


def sampler_for(tex):
    c = tex.get_editor_property('compression_settings')
    if c == unreal.TextureCompressionSettings.TC_NORMALMAP:
        return unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL
    if c == unreal.TextureCompressionSettings.TC_MASKS:
        return unreal.MaterialSamplerType.SAMPLERTYPE_MASKS
    if tex.get_editor_property('srgb'):
        return unreal.MaterialSamplerType.SAMPLERTYPE_COLOR
    return unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR


if unreal.EditorAssetLibrary.does_asset_exist(PATH) and not unreal.EditorAssetLibrary.delete_asset(PATH):
    raise RuntimeError('Cannot delete old ' + PATH)
mat = assets.create_asset('M_TerrainScanned', '/Game/Materials', unreal.Material, unreal.MaterialFactoryNew())

position = node(mat, unreal.MaterialExpressionWorldPosition)
wetness = node(mat, unreal.MaterialExpressionCollectionParameter, collection=unreal.load_asset('/Game/Weather/MPC_Weather'), parameter_name='Wetness')
vertex = node(mat, unreal.MaterialExpressionVertexColor)

samples = {}
for key, (folder, tex_id, tile) in LAYERS.items():
    uv = custom(mat, 'return P.xy / %.1f;' % tile, [('P', position)], unreal.CustomMaterialOutputType.CMOT_FLOAT2)
    for suffix in ('B', 'N', 'ORM'):
        tex = unreal.load_asset('%sT_%s_4K_%s' % (folder, tex_id, suffix))
        if not tex:
            raise RuntimeError('Missing texture %s %s' % (tex_id, suffix))
        s = node(mat, unreal.MaterialExpressionTextureSample, texture=tex, sampler_type=sampler_for(tex))
        connect(uv, '', s, 'UVs')
        samples[key + suffix] = s

# Layer weights: x = grass, y = moss, z = damp ground. Forest floor fills the rest.
weights = custom(mat, """
float2 p = P.xy * 0.0022; float n = 0; float a = 0.5;
for (int i = 0; i < 4; i++) {
    float2 c = floor(p); float2 f = frac(p); f = f * f * (3 - 2 * f);
    float h00 = frac(sin(dot(c, float2(127.1, 311.7))) * 43758.5453);
    float h10 = frac(sin(dot(c + float2(1, 0), float2(127.1, 311.7))) * 43758.5453);
    float h01 = frac(sin(dot(c + float2(0, 1), float2(127.1, 311.7))) * 43758.5453);
    float h11 = frac(sin(dot(c + float2(1, 1), float2(127.1, 311.7))) * 43758.5453);
    n += a * lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
    p = p * 2.03 + 11.7; a *= 0.5;
}
float meadow = V.r * (1 - V.g);
float forest = V.g;
float moss = saturate((n - 0.52) * 5) * (0.35 * meadow + 0.75 * forest);
float grass = saturate(meadow * (1 - moss) * saturate((n + 0.25) * 1.6));
float wet = 1 - smoothstep(-23.0, 16.0, P.z + (n - 0.5) * 14.0);          // waterline is at Z = -25
return float3(grass, moss, wet);
""", [('P', position), ('V', vertex)])

BLEND = 'float g = W.x, m = W.y; float f = saturate(1 - g - m);\n'
color = custom(mat, BLEND + """
float3 c = G * (g * 0.78) + F * f + M * m;
float2 cell = floor(P.xy / 900), blend = frac(P.xy / 900);
blend = blend * blend * (3 - 2 * blend);
float4 h = frac(sin(float4(dot(cell, float2(12.9898, 78.233)), dot(cell + float2(1, 0), float2(12.9898, 78.233)), dot(cell + float2(0, 1), float2(12.9898, 78.233)), dot(cell + 1, float2(12.9898, 78.233)))) * 43758.5453);
float macro = lerp(0.85, 1.12, lerp(lerp(h.x, h.y, blend.x), lerp(h.z, h.w, blend.x), blend.y));
c *= macro;
c = lerp(c, c * float3(0.5, 0.48, 0.45), W.z);       // damp soil is darker
// Bare clay on the faces of bluffs and scarps (vertex blue): ochre, banded by its layers,
// with the forest floor's detail for texture.
float lum = dot(F, float3(0.3, 0.5, 0.2));
float3 clay = float3(0.40, 0.28, 0.17) * saturate(lum * 2.4 + 0.35) * (0.88 + 0.12 * sin(P.z * 0.21 + sin(P.x * 0.01)));
c = lerp(c, clay, V.b);
// After rain the ground darkens (MPC_Weather.Wetness).
return c * lerp(1.0, 0.62, Wt);
""", [('W', weights), ('G', samples['GB']), ('F', samples['FB']), ('M', samples['MB']), ('P', position), ('V', vertex), ('Wt', wetness)])
normal = custom(mat, BLEND + """
float3 n = G * g + F * f + M * m;
n.xy *= lerp(1.0, 0.6, W.z);                         // wet ground looks smoother
n.xy *= lerp(1.0, 0.55, V.b);                        // clay is smoother than leaf litter
return normalize(n);
""", [('W', weights), ('G', samples['GN']), ('F', samples['FN']), ('M', samples['MN']), ('V', vertex)])
rough = custom(mat, BLEND + 'float r = G.g * g + F.g * f + M.g * m;\nr = lerp(r, 0.8, V.b);\nr = lerp(r, 0.35, Wt);\nreturn lerp(r, 0.28, W.z);',
               [('W', weights), ('G', samples['GORM']), ('F', samples['FORM']), ('M', samples['MORM']), ('V', vertex), ('Wt', wetness)],
               unreal.CustomMaterialOutputType.CMOT_FLOAT1)
ao = custom(mat, BLEND + 'return G.r * g + F.r * f + M.r * m;',
            [('W', weights), ('G', samples['GORM']), ('F', samples['FORM']), ('M', samples['MORM'])],
            unreal.CustomMaterialOutputType.CMOT_FLOAT1)

for src, prop in [(color, unreal.MaterialProperty.MP_BASE_COLOR), (normal, unreal.MaterialProperty.MP_NORMAL),
                  (rough, unreal.MaterialProperty.MP_ROUGHNESS), (ao, unreal.MaterialProperty.MP_AMBIENT_OCCLUSION)]:
    if not lib.connect_material_property(src, '', prop):
        raise RuntimeError('Cannot connect %s' % prop)
lib.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(PATH)

# Fab's Megascans master materials ship without the instanced-mesh usage flag; the scene draws
# rocks with instanced components, so without it the engine falls back to the default material.
for master in ('/Game/Fab/Materials/Standard/M_MS_Base_Fuzz', '/Game/Fab/Materials/Standard/M_MS_Base'):
    m = unreal.load_asset(master)
    if m and not m.get_editor_property('used_with_instanced_static_meshes'):
        m.set_editor_property('used_with_instanced_static_meshes', True)
        lib.recompile_material(m)
        unreal.EditorAssetLibrary.save_asset(master)
        unreal.log('Enabled instanced usage on ' + master)
unreal.log('QUIET_LAKE_TERRAIN_MATERIAL_READY')
