"""Materials for the dirt roads and footpaths (run as a UE Python commandlet after CreateWeatherAssets.py).

Both are drawn as strips laid over the terrain; UV.x runs across the strip (0..1), UV.y along it.
M_Road - a forest dirt road: two muddy wheel ruts, a strip of grass between them, trodden earth at
         the sides, ragged grassy edges. Ruts darken and shine when wet (MPC_Weather.Wetness).
M_Path - a footpath: bare trodden earth fading into the grass at its ragged edges.
Textures: the Megascans surfaces already used by the terrain (forest floor, uncut grass).
"""
import unreal

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
SURF = '/Game/Fab/Megascans/Surfaces/'
LAYERS = {
    'G': (SURF + 'Uncut_Grass_oilpt20/High/oilpt20_tier_1/Textures/', 'oilpt20', 220.0),
    'F': (SURF + 'Mossy_Forest_Floor_vfylbge/High/vfylbge_tier_1/Textures/', 'vfylbge', 160.0),
}
F1, F3 = unreal.CustomMaterialOutputType.CMOT_FLOAT1, unreal.CustomMaterialOutputType.CMOT_FLOAT3


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def connect(src, out, dst, inp):
    if not lib.connect_material_expressions(src, out, dst, inp):
        raise RuntimeError('Material link failed: %s.%s -> %s.%s' % (src.get_name(), out, dst.get_name(), inp))


def custom(mat, code, inputs, output=F3):
    n = node(mat, unreal.MaterialExpressionCustom, code=code, output_type=output)
    items = []
    for name, _ in inputs:
        it = unreal.CustomInput()
        it.set_editor_property('input_name', name)
        items.append(it)
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


def build(name, masks, ragged):
    """masks(U) code returns float3(mud, grass, earth); `ragged` is the width of the frayed edges (0..0.5)."""
    path = '/Game/Materials/' + name
    if unreal.EditorAssetLibrary.does_asset_exist(path) and not unreal.EditorAssetLibrary.delete_asset(path):
        raise RuntimeError('Cannot delete ' + path)
    mat = assets.create_asset(name, '/Game/Materials', unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_MASKED)
    wp = node(mat, unreal.MaterialExpressionWorldPosition)
    uv = node(mat, unreal.MaterialExpressionTextureCoordinate)
    wet = node(mat, unreal.MaterialExpressionCollectionParameter, collection=unreal.load_asset('/Game/Weather/MPC_Weather'), parameter_name='Wetness')
    s = {}
    for key, (folder, tex_id, tile) in LAYERS.items():
        coords = custom(mat, 'return P.xy / %.1f;' % tile, [('P', wp)], unreal.CustomMaterialOutputType.CMOT_FLOAT2)
        for suffix in ('B', 'N', 'ORM'):
            tex = unreal.load_asset('%sT_%s_4K_%s' % (folder, tex_id, suffix))
            if not tex:
                raise RuntimeError('Missing texture %s %s' % (tex_id, suffix))
            t = node(mat, unreal.MaterialExpressionTextureSample, texture=tex, sampler_type=sampler_for(tex))
            connect(coords, '', t, 'UVs')
            s[key + suffix] = t
    weights = custom(mat, masks, [('UV', uv), ('P', wp)])
    colour = custom(mat, """
float3 mud = F * float3(0.55, 0.45, 0.36) * lerp(1.0, 0.55, Wt);
float3 earth = F * float3(0.85, 0.75, 0.62);
float3 c = mud * W.x + G * W.y + earth * W.z;
return c / max(0.001, W.x + W.y + W.z);""", [('W', weights), ('F', s['FB']), ('G', s['GB']), ('Wt', wet)])
    normal = custom(mat, 'float3 n = FN * (W.x * 0.5 + W.z) + GN * W.y; n = n / max(0.001, W.x * 0.5 + W.y + W.z); n.xy *= lerp(1.0, 0.4, W.x); return normalize(n);',
                    [('W', weights), ('FN', s['FN']), ('GN', s['GN'])])
    rough = custom(mat, 'float r = (FR.g * (W.x + W.z) + GR.g * W.y) / max(0.001, W.x + W.y + W.z); return lerp(r, lerp(0.55, 0.12, Wt), W.x);',
                   [('W', weights), ('FR', s['FORM']), ('GR', s['GORM']), ('Wt', wet)], F1)
    # Frayed edges: smooth noise makes the edge wander, and a per-pixel dither that temporal
    # anti-aliasing resolves turns the masked edge into a soft blend with the grass.
    opacity = custom(mat, """
float2 q = P.xy / 35.0; float2 c = floor(q); float2 f = frac(q); f = f * f * (3 - 2 * f);
float a = frac(sin(dot(c, float2(12.9898, 78.233))) * 43758.5453);
float b = frac(sin(dot(c + float2(1, 0), float2(12.9898, 78.233))) * 43758.5453);
float d = frac(sin(dot(c + float2(0, 1), float2(12.9898, 78.233))) * 43758.5453);
float g = frac(sin(dot(c + float2(1, 1), float2(12.9898, 78.233))) * 43758.5453);
float n = lerp(lerp(a, b, f.x), lerp(d, g, f.x), f.y);
float alpha = saturate(min(UV.x, 1.0 - UV.x) / %.3f * (0.55 + 0.9 * n) - 0.15);
float2 px = Parameters.SvPosition.xy;
float ign = frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
return alpha > ign ? 1.0 : 0.0;""" % ragged, [('UV', uv), ('P', wp)], F1)
    for src, p in ((colour, unreal.MaterialProperty.MP_BASE_COLOR), (normal, unreal.MaterialProperty.MP_NORMAL),
                   (rough, unreal.MaterialProperty.MP_ROUGHNESS), (opacity, unreal.MaterialProperty.MP_OPACITY_MASK)):
        if not lib.connect_material_property(src, '', p):
            raise RuntimeError('Cannot connect %s' % p)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path)


def build_street(name):
    """An opaque dirt street for the village: two wheel ruts in the same UV.x, no dithered mask."""
    path = '/Game/Materials/' + name
    if unreal.EditorAssetLibrary.does_asset_exist(path) and not unreal.EditorAssetLibrary.delete_asset(path):
        raise RuntimeError('Cannot delete ' + path)
    mat = assets.create_asset(name, '/Game/Materials', unreal.Material, unreal.MaterialFactoryNew())
    wp = node(mat, unreal.MaterialExpressionWorldPosition)
    uv = node(mat, unreal.MaterialExpressionTextureCoordinate)
    wet = node(mat, unreal.MaterialExpressionCollectionParameter, collection=unreal.load_asset('/Game/Weather/MPC_Weather'), parameter_name='Wetness')
    smp = {}
    for key, (folder, tex_id, tile) in LAYERS.items():
        coords = custom(mat, 'return P.xy / %.1f;' % tile, [('P', wp)], unreal.CustomMaterialOutputType.CMOT_FLOAT2)
        for suffix in ('B', 'N', 'ORM'):
            tex = unreal.load_asset('%sT_%s_4K_%s' % (folder, tex_id, suffix))
            if not tex:
                raise RuntimeError('Missing texture %s %s' % (tex_id, suffix))
            tex_node = node(mat, unreal.MaterialExpressionTextureSample, texture=tex, sampler_type=sampler_for(tex))
            connect(coords, '', tex_node, 'UVs')
            smp[key + suffix] = tex_node
    colour = custom(mat, """
float u = UV.x;
float wob = sin(P.x * 0.013 + P.y * 0.011) * 0.035;
float rut = max(1.0 - smoothstep(0.06, 0.13, abs(u - 0.28 - wob)), 1.0 - smoothstep(0.06, 0.13, abs(u - 0.72 - wob)));
float3 earth = F * float3(0.95, 0.82, 0.64);
float3 mud = F * float3(0.58, 0.45, 0.34) * lerp(1.0, 0.6, Wt);
return lerp(earth, mud, rut * 0.9);""", [('UV', uv), ('P', wp), ('F', smp['FB']), ('Wt', wet)])
    normal = custom(mat, 'return float3(FN.x, FN.y * 0.7, FN.z);', [('FN', smp['FN'])])
    rough = custom(mat, 'return lerp(FR.g, lerp(0.8, 0.25, Wt), 0.5);',
                   [('FR', smp['FORM']), ('Wt', wet)], unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    for src, prop in ((colour, unreal.MaterialProperty.MP_BASE_COLOR), (normal, unreal.MaterialProperty.MP_NORMAL),
                      (rough, unreal.MaterialProperty.MP_ROUGHNESS)):
        if not lib.connect_material_property(src, '', prop):
            raise RuntimeError('Cannot connect %s' % prop)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path)


build_street('M_VillageRoad')
# Road: ruts at a quarter of the width from each side, grass in the middle, earth between.
build('M_Road', """
float u = UV.x;
float wob = sin(P.x * 0.013 + P.y * 0.011) * 0.025;
float rut = max(1.0 - smoothstep(0.05, 0.11, abs(u - 0.27 - wob)), 1.0 - smoothstep(0.05, 0.11, abs(u - 0.73 - wob)));
float grass = (1.0 - smoothstep(0.06, 0.12, abs(u - 0.5 - wob))) + smoothstep(0.38, 0.47, abs(u - 0.5)) * 0.8;
float earth = saturate(1.0 - rut - grass) + 0.05;
return float3(rut, saturate(grass) * (1.0 - rut), earth);""", 0.16)
# Path: trodden earth in the middle, thinning grass at the sides.
build('M_Path', """
float u = abs(UV.x - 0.5) * 2.0;
float grass = smoothstep(0.35, 0.95, u);
return float3(0.15 * (1.0 - u), grass, 1.0 - grass);""", 0.3)
unreal.log('QUIET_LAKE_TRACK_MATERIALS_READY')
