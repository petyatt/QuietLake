"""Weathered wood materials for the pier, boards, piles and snags (run as a UE Python commandlet).

M_WeatheredWood - old planks gone silver-brown in the weather; every instance (plank) a slightly
                  different shade, blotchy greying and dark streaks.
M_OldBark       - dark, wet-looking bark and bare wood for piles, logs and branches (engine
                  cylinders): furrows run along the cylinder's axis (local Z).
"""
import unreal

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
FOLDER = '/Game/Materials'

NOISE = """
float vn(float3 p) {
    float3 c = floor(p); float3 f = frac(p); f = f * f * (3 - 2 * f);
    float n000 = frac(sin(dot(c, float3(127.1, 311.7, 74.7))) * 43758.5453);
    float n100 = frac(sin(dot(c + float3(1,0,0), float3(127.1, 311.7, 74.7))) * 43758.5453);
    float n010 = frac(sin(dot(c + float3(0,1,0), float3(127.1, 311.7, 74.7))) * 43758.5453);
    float n110 = frac(sin(dot(c + float3(1,1,0), float3(127.1, 311.7, 74.7))) * 43758.5453);
    float n001 = frac(sin(dot(c + float3(0,0,1), float3(127.1, 311.7, 74.7))) * 43758.5453);
    float n101 = frac(sin(dot(c + float3(1,0,1), float3(127.1, 311.7, 74.7))) * 43758.5453);
    float n011 = frac(sin(dot(c + float3(0,1,1), float3(127.1, 311.7, 74.7))) * 43758.5453);
    float n111 = frac(sin(dot(c + float3(1,1,1), float3(127.1, 311.7, 74.7))) * 43758.5453);
    return lerp(lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y), lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y), f.z);
}
"""


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def custom(mat, body, inputs, output=unreal.CustomMaterialOutputType.CMOT_FLOAT3):
    # Custom nodes cannot declare functions, so the noise is written as a struct with a method.
    code = 'struct N { ' + NOISE.replace('float vn', 'float vn') + ' } nz;\n' + body.replace('vn(', 'nz.vn(')
    n = node(mat, unreal.MaterialExpressionCustom, code=code, output_type=output)
    items = []
    for name, _ in inputs:
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        items.append(item)
    n.set_editor_property('inputs', items)
    for name, src in inputs:
        if not lib.connect_material_expressions(src, '', n, name):
            raise RuntimeError('link failed ' + name)
    return n


def prop(src, p):
    if not lib.connect_material_property(src, '', p):
        raise RuntimeError('Cannot connect %s' % p)


def fresh(name):
    path = FOLDER + '/' + name
    if unreal.EditorAssetLibrary.does_asset_exist(path) and not unreal.EditorAssetLibrary.delete_asset(path):
        raise RuntimeError('Cannot delete ' + path)
    mat = assets.create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property('used_with_instanced_static_meshes', True)
    return mat, path


# --- weathered planks ---------------------------------------------------------------------------
mat, path = fresh('M_WeatheredWood')
wp = node(mat, unreal.MaterialExpressionWorldPosition)
rnd = node(mat, unreal.MaterialExpressionPerInstanceRandom)
color = custom(mat, """
float3 silver = float3(0.13, 0.125, 0.115), brown = float3(0.10, 0.076, 0.055);
float3 c = lerp(brown, silver, saturate(R * 0.8 + vn(P * 0.03) * 0.5));
float grain = vn(P * float3(0.9, 0.9, 0.06)) * 0.5 + vn(P * float3(0.06, 0.9, 0.9)) * 0.5;
c *= lerp(0.78, 1.08, grain);
c *= lerp(0.7, 1.0, smoothstep(0.15, 0.4, vn(P * 0.12 + 7)));   // dark rot blotches
return c;""", [('P', wp), ('R', rnd)])
prop(color, unreal.MaterialProperty.MP_BASE_COLOR)
rough = custom(mat, 'return lerp(0.78, 0.95, vn(P * 0.05));', [('P', wp)], unreal.CustomMaterialOutputType.CMOT_FLOAT1)
prop(rough, unreal.MaterialProperty.MP_ROUGHNESS)
lib.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(path)

# --- old bark --------------------------------------------------------------------------------
mat, path = fresh('M_OldBark')
lp = node(mat, unreal.MaterialExpressionLocalPosition)
wp = node(mat, unreal.MaterialExpressionWorldPosition)
rnd = node(mat, unreal.MaterialExpressionPerInstanceRandom)
nrm = node(mat, unreal.MaterialExpressionVertexNormalWS)
color = custom(mat, """
float a = atan2(L.y, L.x);
float furrow = abs(sin(a * 9 + vn(float3(L.z * 0.05, a, R * 10)) * 3));
float3 bark = lerp(float3(0.07, 0.06, 0.05), float3(0.17, 0.145, 0.115), smoothstep(0.1, 0.9, furrow));
float3 grey = float3(0.13, 0.125, 0.115);
float3 c = lerp(bark, grey, saturate(R * 0.4 + vn(W * 0.02) * 0.3 - 0.25));
float wet = 1 - saturate((W.z + 22) / 20);                  // darker where it has been in the water
c = lerp(c, c * float3(0.45, 0.45, 0.42), wet);
// Moss on the upper side of lying logs and snags, in patches.
float moss = saturate(Nw.z * 1.4 - 0.3) * smoothstep(0.35, 0.65, vn(W * 0.03 + 7)) * (1 - wet);
c = lerp(c, float3(0.045, 0.06, 0.025), moss * 0.8);
// Old wood stays dark under the bright sky the scene is exposed for.
return c * 0.6 * lerp(0.85, 1.1, vn(W * 0.08));""", [('L', lp), ('W', wp), ('R', rnd), ('Nw', nrm)])
prop(color, unreal.MaterialProperty.MP_BASE_COLOR)
rough = custom(mat, 'float wet = 1 - saturate((W.z + 22) / 20); return lerp(0.9, 0.45, wet);', [('W', wp)], unreal.CustomMaterialOutputType.CMOT_FLOAT1)
prop(rough, unreal.MaterialProperty.MP_ROUGHNESS)
lib.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(path)
unreal.log('QUIET_LAKE_WOOD_READY')
