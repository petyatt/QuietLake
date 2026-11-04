"""Create original procedural materials and import our synthesized ambience.

QUIET_LAKE_ONLY=M_LakeSurface,... (environment variable): rebuild only these materials and skip
the audio, so tuned assets made by other scripts stay as they are.
"""
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
    # An input is (name, source) or (name, source, output pin), e.g. 'RGBA' for all four
    # components of a vector parameter.
    for entry in inputs:
        item = unreal.CustomInput()
        item.set_editor_property('input_name', entry[0])
        entries.append(item)
    node.set_editor_property('inputs', entries)
    for entry in inputs:
        connect(entry[1], entry[2] if len(entry) > 2 else '', node, entry[0])
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

# The lake surface by the weather (MPC_Weather): Wind 0..1 (12 m/s), WindDirX/Y (where it blows),
# Rain 0..1, Wetness (how long it has rained). Returns float4(normal, roughness).
#  calm      - a mirror with a faint long swell;
#  light air - patches of ripples ("cat's paws") drifting downwind between glassy streaks;
#  breeze    - ripples everywhere, wavelets running downwind, reflections broken, far water matt;
#  strong    - short steep waves (whitecaps come from WATER_FOAM);
#  rain      - drizzle: sparse rings; downpour: dense splashes and a grey, matt surface.
# Waves travel at their real speed (gravity and surface tension); what is too fine to draw far
# away becomes roughness instead.
WATER_COMMON = """
float2 wd = normalize(float2(X, Y) + float2(1e-4, 0));
float dist = distance(P, C);
// Gusts: slow noise drifting downwind. In light air it leaves bare patches between ripples.
float2 gp = (P.xy - wd * T * lerp(60.0, 420.0, W)) / 2600.0;
float gust = 0.0, ga = 0.5;
for (int o = 0; o < 3; o++)
{
    float2 c = floor(gp), f = frac(gp);
    f = f * f * (3.0 - 2.0 * f);
    float h00 = frac(sin(dot(c, float2(127.1, 311.7))) * 43758.5453);
    float h10 = frac(sin(dot(c + float2(1, 0), float2(127.1, 311.7))) * 43758.5453);
    float h01 = frac(sin(dot(c + float2(0, 1), float2(127.1, 311.7))) * 43758.5453);
    float h11 = frac(sin(dot(c + float2(1, 1), float2(127.1, 311.7))) * 43758.5453);
    gust += ga * lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
    gp = gp * 2.13 + 5.7;
    ga *= 0.5;
}
gust /= 0.875;
float patch = lerp(smoothstep(0.5, 0.72, gust), 0.55 + 0.45 * gust, smoothstep(0.2, 0.55, W));
"""
WATER_SURFACE = WATER_COMMON + """
float lens[8] = { 420, 240, 140, 85, 52, 31, 19, 11 };
float angs[8] = { 0, 28, -32, 12, -48, 55, -18, 75 };       // degrees off the wind
float phase[8] = { 0.0, 1.7, 4.1, 2.3, 5.2, 0.9, 3.6, 2.8 };
float2 g = 0;
float lost = 0;      // slope too fine to draw at this distance: it becomes roughness
for (int i = 0; i < 8; i++)
{
    float a = radians(angs[i]);
    float2 d = float2(wd.x * cos(a) - wd.y * sin(a), wd.x * sin(a) + wd.y * cos(a));
    float k = 6.2831853 / lens[i];
    float w = sqrt(980.0 * k + 74.0 * k * k * k);           // gravity-capillary dispersion
    // Long waves need a strong, steady wind; ripples answer every breath of it.
    float s = i < 2 ? 0.07 * pow(W, 1.6)
            : i < 5 ? 0.05 * W * lerp(0.6, 1.0, patch)
            : 0.045 * saturate(W * 5.0) * patch;
    // Detail fades out with distance, the shortest first.
    float keep = 1.0 - saturate(dist / (lens[i] * 25.0));
    g += d * s * keep * cos(dot(d, P.xy) * k - w * T + phase[i]);
    lost += s * (1.0 - keep);
}
// Even a calm lake breathes: a faint long swell.
g += float2(0.6, 0.8) * 0.004 * cos(dot(float2(0.6, 0.8), P.xy) * 0.0105 - T * 0.9);
// Rain: every cell of a grid gets drops at its own moments; each sends out a ring. Drizzle lights
// a few cells, a downpour nearly all of them, at three scales.
float rainRough = 0;
if (R > 0.01)
{
    float near = 1.0 - saturate(dist / 2500);
    for (int l = 0; l < 3; l++)
    {
        float cell = l == 0 ? 30.0 : l == 1 ? 18.0 : 45.0;
        float2 q = P.xy / cell + float2(l * 0.37, l * 0.61);
        float2 c = floor(q);
        float h = frac(sin(dot(c + l * 7.1, float2(12.9898, 78.233))) * 43758.5453);
        float h2 = frac(h * 17.31 + 0.13);
        if (h2 > R * R * 0.9 + 0.1 * R) continue;
        float t = frac(T * (1.1 + 0.6 * h) + h);
        float2 v = q - (c + 0.25 + 0.5 * float2(h, frac(h * 7.7)));
        float d = length(v);
        float ring = d - t * 0.45;
        float wave = sin(ring * 45.0) * exp(-ring * ring * 300.0) * (1.0 - t);
        g += (v / max(d, 1e-3)) * wave * 0.5 * near;
    }
    rainRough = 0.35 * R * R + 0.05 * R;
}
// Waves simulated near the viewer (ALakeRipples): a height field in cm on a square window,
// RW = (origin x, origin y, size, one texel in UV). Its slope adds to the ripples; it fades out
// towards the window edge.
if (RW.z > 0)
{
    float2 ruv = (P.xy - RW.xy) / RW.z;
    float edge = saturate(min(min(ruv.x, ruv.y), min(1.0 - ruv.x, 1.0 - ruv.y)) * 14.0);
    if (edge > 0)
    {
        float e = RW.w;
        float hl = Texture2DSampleLevel(Rip, RipSampler, ruv - float2(e, 0), 0).r;
        float hr = Texture2DSampleLevel(Rip, RipSampler, ruv + float2(e, 0), 0).r;
        float hd = Texture2DSampleLevel(Rip, RipSampler, ruv - float2(0, e), 0).r;
        float hu = Texture2DSampleLevel(Rip, RipSampler, ruv + float2(0, e), 0).r;
        g += float2(hr - hl, hu - hd) / (2.0 * e * RW.z) * edge;
    }
}
float rough = 0.02 + lost * 3.0 + 0.04 * W + rainRough;
return float4(normalize(float3(-g, 1)), saturate(rough));
"""
# Whitecaps in a strong wind: streaks of foam along the wind on the crests, in the gusts.
WATER_FOAM = WATER_COMMON + """
if (W < 0.55) return 0;
float2 side = float2(-wd.y, wd.x);
float along = dot(P.xy, wd), across = dot(P.xy, side);
float crest = sin(along * 6.2831853 / 240.0 - T * 4.0 + sin(across * 0.004) * 2.0) * 0.5 + 0.5;
float streak = frac(sin(floor(across / 35.0) * 12.9898) * 43758.5453);
float foam = smoothstep(0.82, 0.97, crest) * smoothstep(0.55, 0.9, gust) * step(0.45, streak);
return foam * smoothstep(0.55, 0.9, W) * (1.0 - saturate(distance(P, C) / 9000)) * 0.8;
"""

weather = unreal.load_asset('/Game/Weather/MPC_Weather')
only = [n for n in os.environ.get('QUIET_LAKE_ONLY', '').split(',') if n]
if not weather:
    raise RuntimeError('Run CreateWeatherAssets.py first: MPC_Weather is missing')
for name, rgb in palette.items():
    if only and name not in only:
        continue
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
        param = lambda n: expression(mat, unreal.MaterialExpressionCollectionParameter, collection=weather, parameter_name=n)
        rain, wind, wx, wy, wetness = param('Rain'), param('Wind'), param('WindDirX'), param('WindDirY'), param('Wetness')
        inputs = [('P', position), ('T', time), ('C', camera), ('R', rain), ('W', wind), ('X', wx), ('Y', wy)]
        # The simulated waves (set at run time by ALakeRipples; a black texture and a zero window
        # leave the water as it was).
        ripples = expression(mat, unreal.MaterialExpressionTextureObjectParameter, parameter_name='Ripples',
                             texture=unreal.load_asset('/Engine/EngineResources/Black'))
        window = expression(mat, unreal.MaterialExpressionVectorParameter, parameter_name='RippleWindow',
                            default_value=unreal.LinearColor(0, 0, 0, 0))
        surface = custom(mat, WATER_SURFACE, inputs + [('Rip', ripples), ('RW', window, 'RGBA')], unreal.CustomMaterialOutputType.CMOT_FLOAT4)
        normal = expression(mat, unreal.MaterialExpressionComponentMask, r=True, g=True, b=True, a=False)
        rough = expression(mat, unreal.MaterialExpressionComponentMask, r=False, g=False, b=False, a=True)
        connect(surface, '', normal, '')
        connect(surface, '', rough, '')
        lib.connect_material_property(normal, '', unreal.MaterialProperty.MP_NORMAL)
        lib.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)
        # Foam: in single-layer water the base colour is a lit surface layer whose coverage is the
        # Opacity input (unconnected it is 1 and hides the whole water body - the lake was a dark
        # film). Only the foam covers the water; everywhere else one sees into it.
        foam = custom(mat, WATER_FOAM, inputs, unreal.CustomMaterialOutputType.CMOT_FLOAT1)
        base = expression(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.6, 0.62, 0.63, 1))
        lib.connect_material_property(base, '', unreal.MaterialProperty.MP_BASE_COLOR)
        lib.connect_material_property(foam, '', unreal.MaterialProperty.MP_OPACITY)
        out = expression(mat, unreal.MaterialExpressionSingleLayerWaterMaterialOutput)
        # A forest lake with a little peat: the humus in the water eats blue light most, then green,
        # red least, so the water is tea-amber over a sandy bottom and dark brown-green in the deep;
        # fine silt and plankton scatter a little light back (greenish-yellow). The engine takes the
        # coefficients per centimetre (scene depth); below they are written per metre / 100.
        # Visibility is about a metre: a fish 15 cm down is seen but already tinted, 40 cm down a dim
        # shadow, 80 cm down next to nothing; the sandy shallows by the bank stay clear. (Much more
        # scattering turns the lake into opaque green paint.) After long rain the runoff clouds it.
        scatter = custom(mat, 'return (float3(0.08, 0.1, 0.045) * (1.0 + 3.0 * Wt) + float3(0.15, 0.14, 0.08) * Wt) / 100.0;', [('Wt', wetness)])
        absorb = expression(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.012, 0.013, 0.032, 1))
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

# Under the water (post process, switched on by ALakeLife only while the camera is below the
# surface): the lake swallows the view within a metre or two, fading into olive-brown murk whose
# brightness follows what the camera sees (day, dusk, night). Extinction per centimetre.
UNDERWATER = """
float3 avg = 0;
for (int i = 0; i < 5; i++)
    for (int j = 0; j < 3; j++)
        avg += SceneTextureLookup(ViewportUVToBufferUV(float2((i + 0.5) / 5.0, (j + 0.5) / 3.0)), 14, false).rgb;
avg /= 15.0;
float lum = dot(avg, float3(0.3, 0.59, 0.11));
float3 murk = float3(0.42, 0.5, 0.26) * lum;
float3 t = exp(-float3(0.016, 0.015, 0.03) * D);
return C * t + murk * (1.0 - t);
"""
if not only or 'M_Underwater' in only:
    path = '/Game/Materials/M_Underwater'
    if unreal.EditorAssetLibrary.does_asset_exist(path) and not unreal.EditorAssetLibrary.delete_asset(path):
        raise RuntimeError('Cannot delete old material: ' + path)
    mat = assets.create_asset('M_Underwater', '/Game/Materials', unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property('material_domain', unreal.MaterialDomain.MD_POST_PROCESS)
    mat.set_editor_property('blendable_location', unreal.BlendableLocation.BL_SCENE_COLOR_BEFORE_DOF)
    scene = expression(mat, unreal.MaterialExpressionSceneTexture, scene_texture_id=unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0)
    depth = expression(mat, unreal.MaterialExpressionSceneDepth)
    murk = custom(mat, UNDERWATER, [('C', scene), ('D', depth)])
    lib.connect_material_property(murk, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path)

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The ambience (skipped when only some materials are rebuilt).
if not only:
    # The old surf-like water loop and the first rain loop are no longer used.
    for old in ('A_Water', 'A_Rain'):
        if unreal.EditorAssetLibrary.does_asset_exist('/Game/Audio/' + old):
            unreal.EditorAssetLibrary.delete_asset('/Game/Audio/' + old)
    for name in ('A_Wind','A_Birds','A_RainLight','A_RainHeavy','A_Crickets'):
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
unreal.log('QUIET_LAKE_WORLD_ASSETS_READY' + (' (only %s)' % ', '.join(only) if only else ''))
