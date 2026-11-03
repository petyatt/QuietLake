"""Assets for the life of the pond (run as a UE Python commandlet after CreateWeatherAssets.py, which
makes MPC_Weather; sounds from Tools/GenerateLifeSounds.py).

M_WaterSign  - signs on the water drawn on a flat quad (the engine's plane) over the surface:
               rings of a rise, bubbles, a swirl, a splash's foam and rings, fry spraying.
               Per-instance custom data: 0 = start time (s, game time), 1 = kind (ELakeSign),
               2 = random seed, 3 = size. Everything animates in the material from the start time.
M_WaterSpray - a crown of drops thrown up by a splash, drawn on vertical quads.
               Custom data: 0 = start time, 1 = size, 2 = seed.
A_Plop, A_Bubbles, A_Splash, A_Scatter - one-shot sounds.
"""
import os
import unreal

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = '/Game/Life'
F1, F4 = unreal.CustomMaterialOutputType.CMOT_FLOAT1, unreal.CustomMaterialOutputType.CMOT_FLOAT4
mpc = unreal.load_asset('/Game/Weather/MPC_Weather')
if not mpc:
    raise RuntimeError('Run CreateWeatherAssets.py first (MPC_Weather)')


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def custom(mat, code, inputs, output):
    n = node(mat, unreal.MaterialExpressionCustom, code=code, output_type=output)
    items = []
    for name, _, _ in inputs:
        it = unreal.CustomInput()
        it.set_editor_property('input_name', name)
        items.append(it)
    n.set_editor_property('inputs', items)
    for name, src, pin in inputs:
        if not lib.connect_material_expressions(src, pin, n, name):
            raise RuntimeError('link failed ' + name)
    return n


def new_material(name, two_sided):
    path = FOLDER + '/' + name
    if unreal.EditorAssetLibrary.does_asset_exist(path) and not unreal.EditorAssetLibrary.delete_asset(path):
        raise RuntimeError('Cannot delete ' + path)
    mat = assets.create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property('two_sided', two_sided)
    mat.set_editor_property('used_with_instanced_static_meshes', True)
    return mat, path


def finish(mat, path, rgba):
    rgb = node(mat, unreal.MaterialExpressionComponentMask, r=True, g=True, b=True, a=False)
    alpha = node(mat, unreal.MaterialExpressionComponentMask, r=False, g=False, b=False, a=True)
    lib.connect_material_expressions(rgba, '', rgb, '')
    lib.connect_material_expressions(rgba, '', alpha, '')
    if not lib.connect_material_property(rgb, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        raise RuntimeError('emissive')
    if not lib.connect_material_property(alpha, '', unreal.MaterialProperty.MP_OPACITY):
        raise RuntimeError('opacity')
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path)


def common_inputs(mat):
    uv = node(mat, unreal.MaterialExpressionTextureCoordinate)
    tm = node(mat, unreal.MaterialExpressionTime)
    day = node(mat, unreal.MaterialExpressionCollectionParameter, collection=mpc, parameter_name='Daylight')
    data = [node(mat, unreal.MaterialExpressionPerInstanceCustomData, data_index=i) for i in range(4)]
    return uv, tm, day, data


# --- signs on the surface --------------------------------------------------------------------
SIGN = """
float2 p = (UV - 0.5) * 2.0;
float d = length(p);
float age = T - Start;
int kind = (int)round(Kind);
if (age < 0.0 || age > 7.0 || d > 1.0) return float4(0, 0, 0, 0);
float a = 0.0;      // thin rings: the sky glinting on the slopes of a wave
float foam = 0.0;   // bubbles, foam, drops: bright and solid
if (kind == 0)
{
    // Rise: three rings spreading from the centre.
    for (int i = 0; i < 3; i++)
    {
        float t = age - i * 0.28;
        if (t > 0.0)
        {
            float r = t * 0.34 + 0.03;
            float q = (d - r) / (0.02 + 0.03 * r);
            a += exp(-q * q) * saturate(1.0 - age / 3.0) * (1.0 - i * 0.28) * (1.0 - smoothstep(0.75, 1.0, r));
        }
    }
}
else if (kind == 1)
{
    // Bubbles: one after another they come up in a wandering trail, sit a moment and pop.
    for (int i = 0; i < 12; i++)
    {
        float hx = frac(sin((Seed + i) * 78.233) * 43758.5453);
        float hy = frac(sin((Seed + i) * 39.425) * 24634.6345);
        float ht = frac(sin((Seed + i) * 17.719) * 12345.6789);
        float2 c = (float2(hx, hy) - 0.5) * 1.1;
        float tb = age - ht * 4.0;
        float bd = length(p - c);
        float br = 0.03 + 0.025 * frac(hx * 7.0);
        if (tb > 0.0 && tb < 1.1) foam += smoothstep(br, br * 0.35, bd) * smoothstep(0.0, 0.08, tb) * (1.0 - smoothstep(0.9, 1.1, tb)) * 0.8;
        float tp = tb - 1.1;
        if (tp > 0.0 && tp < 0.9)
        {
            float q = (bd - (tp * 0.25 + br)) / 0.015;
            a += exp(-q * q) * (1.0 - tp / 0.9) * 0.8;
        }
    }
}
else if (kind == 2)
{
    // Swirl: a soft bulge of water lifting, then wide slow rings.
    float qb = (d - 0.18 - age * 0.15) / 0.12;
    a += exp(-qb * qb) * saturate(1.0 - age / 1.0) * 0.9;
    for (int i = 0; i < 3; i++)
    {
        float t = age - i * 0.3;
        if (t > 0.0)
        {
            float r = t * 0.26 + 0.08;
            float q = (d - r) / (0.03 + 0.03 * r);
            a += exp(-q * q) * saturate(1.0 - age / 4.0) * (1.0 - i * 0.25) * (1.0 - smoothstep(0.8, 1.0, r));
        }
    }
}
else if (kind == 3)
{
    // Splash: a patch of ragged foam that breaks up and melts, and quick rings.
    float n = 0.0;
    float amp = 0.6;
    float2 g = p * 6.0 + Seed;
    for (int o = 0; o < 2; o++)
    {
        float2 gi = floor(g);
        float2 gf = frac(g);
        gf = gf * gf * (3.0 - 2.0 * gf);
        float h00 = frac(sin(dot(gi, float2(12.9898, 78.233))) * 43758.5453);
        float h10 = frac(sin(dot(gi + float2(1, 0), float2(12.9898, 78.233))) * 43758.5453);
        float h01 = frac(sin(dot(gi + float2(0, 1), float2(12.9898, 78.233))) * 43758.5453);
        float h11 = frac(sin(dot(gi + float2(1, 1), float2(12.9898, 78.233))) * 43758.5453);
        n += lerp(lerp(h00, h10, gf.x), lerp(h01, h11, gf.x), gf.y) * amp;
        g = g * 2.7 + 3.1;
        amp *= 0.65;
    }
    float fr = 0.1 + 0.25 * saturate(age * 3.0);
    float cut = 0.38 + 0.4 * saturate(age / 2.0);
    foam += (1.0 - smoothstep(fr * 0.5, fr, d)) * smoothstep(cut, cut + 0.12, n) * saturate(1.3 - age / 2.0) * 0.75;
    for (int i = 0; i < 3; i++)
    {
        float t = age - i * 0.22;
        if (t > 0.0)
        {
            float r = t * 0.4 + 0.12;
            float q = (d - r) / (0.025 + 0.03 * r);
            a += exp(-q * q) * saturate(1.0 - age / 3.5) * (1.0 - i * 0.25) * (1.0 - smoothstep(0.8, 1.0, r));
        }
    }
}
else
{
    // Scatter: fry spraying out: many tiny drops and rings over a wide patch.
    for (int i = 0; i < 24; i++)
    {
        float hx = frac(sin((Seed + i) * 78.233) * 43758.5453);
        float hy = frac(sin((Seed + i) * 39.425) * 24634.6345);
        float ht = frac(sin((Seed + i) * 17.719) * 12345.6789);
        float2 c = (float2(hx, hy) - 0.5) * 1.5;
        c *= min(1.0, 0.75 / max(length(c), 1e-3));
        float t = age - ht;
        float bd = length(p - c);
        if (t > 0.0 && t < 1.2)
        {
            float q = (bd - (0.01 + t * 0.12)) / 0.012;
            a += exp(-q * q) * (1.0 - t / 1.2);
            foam += smoothstep(0.022, 0.0, bd) * saturate(1.0 - t * 4.0);
        }
    }
}
float edge = 1.0 - smoothstep(0.85, 1.0, d);
a *= edge * saturate(Size * 4.0);   // the tiniest dimples are faint
foam = saturate(foam) * edge;
float light = 0.08 + 0.92 * Day;
// Unlit, so kept near the brightness of the sky in the water (the scene is exposed for a soft sun):
// thin rings a little brighter than the water, foam soft white.
float op = saturate(a * 0.5 + foam * 0.85);
float3 col = (float3(0.72, 0.78, 0.82) * 0.75 * a * 0.5 + float3(0.93, 0.95, 0.96) * 0.3 * foam * 0.85) / max(op, 1e-4);
return float4(col * light, op);
"""

mat, path = new_material('M_WaterSign', False)
uv, tm, day, data = common_inputs(mat)
rgba = custom(mat, SIGN, [('UV', uv, ''), ('T', tm, ''), ('Start', data[0], ''), ('Kind', data[1], ''), ('Seed', data[2], ''), ('Size', data[3], ''), ('Day', day, '')], F4)
finish(mat, path, rgba)

# --- the crown of drops ----------------------------------------------------------------------
SPRAY = """
float age = T - Start;
if (age < 0.0 || age > 1.6) return float4(0, 0, 0, 0);
float2 q = float2(UV.x - 0.5, 1.0 - UV.y);   // across, and up from the water
float a = 0.0;
for (int i = 0; i < 26; i++)
{
    float h1 = frac(sin((Seed + i) * 78.233) * 43758.5453);
    float h2 = frac(sin((Seed + i) * 39.425) * 24634.6345);
    float h3 = frac(sin((Seed + i) * 17.719) * 12345.6789);
    float t = age - h3 * 0.12;
    if (t < 0.0) continue;
    float2 c = float2((h1 - 0.5) * 0.9 * t, (1.2 + 1.5 * h2) * t - 2.25 * t * t);
    if (c.y < 0.0) continue;
    float r = 0.02 + 0.015 * h3;
    a += smoothstep(r, r * 0.3, length(q - c));
}
// The sheet of water thrown straight up at the first moment.
a += (1.0 - smoothstep(0.03, 0.09, abs(q.x))) * step(q.y, 0.55 * saturate(age * 5.0)) * saturate(1.0 - age * 2.5) * 0.45;
float light = 0.08 + 0.92 * Day;
return float4(float3(0.9, 0.93, 0.95) * light * 0.35, saturate(a) * 0.8);
"""
mat, path = new_material('M_WaterSpray', True)
uv, tm, day, data = common_inputs(mat)
rgba = custom(mat, SPRAY, [('UV', uv, ''), ('T', tm, ''), ('Start', data[0], ''), ('Seed', data[2], ''), ('Day', day, '')], F4)
finish(mat, path, rgba)

# --- sounds ----------------------------------------------------------------------------------
for name in ('A_Plop', 'A_Bubbles', 'A_Splash', 'A_Scatter'):
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', os.path.join(ROOT, 'Tools', 'GeneratedAudio', name + '.wav'))
    task.set_editor_property('destination_path', FOLDER)
    task.set_editor_property('automated', True)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('save', True)
    assets.import_asset_tasks([task])
    sound = unreal.load_asset(FOLDER + '/' + name)
    if not sound:
        raise RuntimeError('Audio import failed: ' + name)
    sound.set_editor_property('looping', False)
    unreal.EditorAssetLibrary.save_asset(FOLDER + '/' + name)
unreal.log('QUIET_LAKE_LIFE_ASSETS_READY')
