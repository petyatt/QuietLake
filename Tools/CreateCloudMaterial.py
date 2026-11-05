"""Build M_LakeClouds, the material of the volumetric cloud layer (run as a UE Python commandlet).

The density is worked out per sample in HLSL from our Perlin-Worley noises (Tools/GenerateCloudNoise.py,
ImportCloudNoise.py; as in Horizon Zero Dawn's clouds): a low-frequency shape noise gives the billows, a height profile per
cloud kind gives the silhouette, a high-frequency erosion noise frays the edges. Three kinds mix
by the weather (ALakeWeather sets the parameters):
  Cumulus - fair-weather heaps, flat bases at ~1.2 km, rounded tops, taller with more of them;
  Stratus - a low grey deck at ~1-2 km, wide and soft, with ragged undersides in the rain;
  Storm   - towering, dark rain clouds from ~1 km up to ~9 km with an anvil spreading at the top.
Density scales them all; WindOffset (km x, y and an evolution phase z) drifts and slowly reshapes
them. A weather change comes as a front: Cumulus/Stratus/Storm/Density are the weather ahead of it
(the one now overhead), the same names with 2 the coming one behind it; Front is its direction of
travel (x, y), the distance of its line from the lake along it (z, km) and the half width of the
change (w, km). The layer itself (bottom 0.8 km, 9 km thick) is set by ALakeWeather.
"""
import unreal

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
PATH = '/Game/Weather/M_LakeClouds'


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def link(src, out, dst, inp):
    if not lib.connect_material_expressions(src, out, dst, inp):
        raise RuntimeError('Material link failed: %s.%s -> %s.%s' % (src.get_name(), out, dst.get_name(), inp))


def custom(mat, code, inputs, output=unreal.CustomMaterialOutputType.CMOT_FLOAT1):
    n = node(mat, unreal.MaterialExpressionCustom, code=code, output_type=output)
    items = []
    for entry in inputs:
        it = unreal.CustomInput()
        it.set_editor_property('input_name', entry[0])
        items.append(it)
    n.set_editor_property('inputs', items)
    for entry in inputs:
        link(entry[1], entry[2] if len(entry) > 2 else '', n, entry[0])
    return n


# Density of the cloud at a sample. P: world position (cm); H: height above the layer's bottom
# (km); Cu, St, Sm: how much of each kind; WO: wind offset (km) and evolution phase.
DENSITY = r"""
float3 pk = P / 100000.0;                         // km
float3 w = float3(WO.x, WO.y, 0);
float h = H;

// The front: the old weather's clouds thin out across its whole width (as the light and rain
// change by the water), the coming ones arrive with a sharp leading edge a little ahead of its
// middle, and where they bring rain a wall of storm towers stands on that edge.
float fx = Fr.z - dot(pk.xy, Fr.xy);
float fOld = 1.0 - smoothstep(-Fr.w, Fr.w, fx);
float fNew = smoothstep(-0.15 * Fr.w, 0.15 * Fr.w, fx + 0.3 * Fr.w);
float wall = fNew * (1.0 - fNew) * 4.0;
float kCu = max(Cu * fOld, Cu2 * fNew), kSt = max(St * fOld, St2 * fNew);
float kSm = max(Sm * fOld, Sm2 * saturate(fNew + wall * 0.6));
float kD = lerp(D, D2, fNew);

// Where clouds gather: broad patches drifting with the wind (from the same noise, as 2D).
float patch = Shape.SampleLevel(ShapeSampler, float3((pk.xy + w.xy * 0.7) / 23.0, 0.31 + WO.z * 0.002), 0).r;
float patch2 = Shape.SampleLevel(ShapeSampler, float3((pk.xy + w.xy * 0.5) / 47.0, 0.71), 0).r;

// The billows: Perlin-Worley shape noise, wider than tall, slowly reshaping with WO.z.
float3 su = float3((pk.xy + w.xy) / 3.2, h / 1.8 + WO.z * 0.01);
float4 s = Shape.SampleLevel(ShapeSampler, su, 0);
float fbm = s.g * 0.625 + s.b * 0.25 + s.a * 0.125;
float shape = saturate((s.r - (fbm - 1.0)) / (2.0 - fbm));     // remap(r, fbm-1, 1, 0, 1)

// Cumulus: separate heaps on cells ~2 km apart (more cells fill in as Cu grows), each with a flat
// base at ~0.45 km over the layer and a rounded top, the strongest cells towering highest.
float4 cl = Shape.SampleLevel(ShapeSampler, float3((pk.xy + w.xy) / 9.0, 0.5 + WO.z * 0.003), 0);
float cells = saturate(cl.r * 0.55 + cl.g * 0.45 + (patch - 0.5) * 0.35);
float cc = saturate(kCu * 0.65);
float cov = saturate((cells - (1.0 - cc)) / max(cc, 0.05));
float top = 0.75 + (0.6 + 2.6 * cc) * cov;
float pcu = smoothstep(0.38, 0.5, h) * saturate((top - h) / max(0.45 * top, 0.05));
float g = cov * pcu;
float dcu = saturate((shape - (1.0 - g)) / max(g, 0.05)) * g;

// Stratus: a wide grey deck from ~0.3 to ~1.1 km with a flat top and a lumpy underside (the cells
// hang lower here and there, like stratocumulus seen from below), ragged in the rain.
float sc = saturate(kSt * (0.8 + 0.4 * patch2));
float4 s2 = Shape.SampleLevel(ShapeSampler, float3((pk.xy + w.xy) / 6.0, h / 1.5 + WO.z * 0.006), 0);
float fbm2 = s2.g * 0.625 + s2.b * 0.25 + s2.a * 0.125;
float shape2 = saturate((s2.r - (fbm2 - 1.0)) / (2.0 - fbm2));
float bottom = 0.5 - 0.3 * cells;                                // lumps of the underside
float pst = smoothstep(bottom, bottom + 0.2, h) * (1.0 - smoothstep(0.95, 1.3, h));
float gs = sc * pst;
float dst = saturate((lerp(shape2, 1.0, 0.35) - (1.0 - gs)) / max(gs, 0.05)) * gs;

// Storm clouds: dark, heavy towers from ~0.2 km up to ~8 km, an anvil spreading under the top;
// twice as dense as the rest, so their bases are dark.
float mc = saturate(kSm * (0.35 + 1.0 * patch2));
float psm = smoothstep(0.1, 0.35, h) * (1.0 - smoothstep(6.8, 8.2, h));
float anvil = smoothstep(5.5, 6.8, h) * (1.0 - smoothstep(7.2, 8.2, h));
float tower = saturate(1.25 - h / 9.0);
float gm = mc * psm * lerp(tower, 1.0, anvil);
float dsm = saturate((lerp(shape, 1.0, 0.25) - (1.0 - gm)) / max(gm, 0.05)) * gm * 2.0;

float d = max(max(dcu, dst), dsm);
if (d <= 0.0) return 0.0;

// Fray the edges with the erosion noise: wispy at the bottom, billowy at the top.
float3 eu = (pk + w * 1.25) / 2.0 + float3(0, 0, WO.z * 0.03);
float3 e3 = Erosion.SampleLevel(ErosionSampler, eu, 0).rgb;
float e = e3.r * 0.625 + e3.g * 0.25 + e3.b * 0.125;
e = lerp(1.0 - e, e, saturate(h * 1.5));
d = saturate((d - e * 0.22) / 0.78);
// Firmer edges and denser cores: the billows read as solid heaps, not as haze.
d = saturate(d * 2.0);
return d * kD;
"""

# How dark the cloud is: storm clouds and thick decks absorb more and scatter less, storm bases most.
ALBEDO = r"""
float fx = Fr.z - dot(P.xy / 100000.0, Fr.xy);
float fOld = 1.0 - smoothstep(-Fr.w, Fr.w, fx);
float fNew = smoothstep(-0.15 * Fr.w, 0.15 * Fr.w, fx + 0.3 * Fr.w);
float kSt = max(St * fOld, St2 * fNew), kSm = max(Sm * fOld, Sm2 * fNew);
// Grey decks absorb a little; storm clouds much more, most of all in their low, rain-laden bases,
// so a storm seen from below is dark even with the sun high over it.
float3 c = lerp(float3(0.96, 0.96, 0.97), float3(0.62, 0.65, 0.70), saturate(kSt * 0.5));
float3 storm = lerp(float3(0.16, 0.18, 0.22), float3(0.50, 0.53, 0.58), saturate((H - 0.3) / 4.0));
return lerp(c, storm, saturate(kSm * 1.1));
"""

# Light the cloud gives off itself, per metre and in proportion to its density (as the engine adds
# it): the flash of a strike inside the storm, brightest low in the cloud and within ~2 km of it
# (Flash: x, y km and strength), and at night a faint moonlit glow, brighter on the tops (NightGlow;
# the moon is no atmosphere light, so it does not light the clouds by itself).
EMISSIVE = r"""
float2 dxy = P.xy / 100000.0 - Fl.xy;
float g = Fl.z * exp(-dot(dxy, dxy) / 4.0) * smoothstep(0.0, 0.5, H) * (1.0 - smoothstep(4.0, 8.0, H));
float3 flash = float3(0.80, 0.86, 1.0) * g;
float3 moon = float3(0.55, 0.62, 0.80) * Ng * (0.35 + 0.65 * saturate(H / 2.5));
return Dn * (flash + moon);
"""

if unreal.EditorAssetLibrary.does_asset_exist(PATH):
    mat = unreal.load_asset(PATH)
    lib.delete_all_material_expressions(mat)
else:
    mat = assets.create_asset('M_LakeClouds', '/Game/Weather', unreal.Material, unreal.MaterialFactoryNew())
mat.set_editor_property('material_domain', unreal.MaterialDomain.MD_VOLUME)
mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_ADDITIVE)
mat.set_editor_property('used_with_volumetric_cloud', True)

where = node(mat, unreal.MaterialExpressionWorldPosition)
cloud = node(mat, unreal.MaterialExpressionCloudSampleAttribute)
shape = node(mat, unreal.MaterialExpressionTextureObjectParameter, parameter_name='ShapeNoise',
             texture=unreal.load_asset('/Game/Weather/Noise/VT_CloudShape'))
erosion = node(mat, unreal.MaterialExpressionTextureObjectParameter, parameter_name='ErosionNoise',
               texture=unreal.load_asset('/Game/Weather/Noise/VT_CloudErosion'))
for t in (shape, erosion):
    t.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
scalar = lambda n, v: node(mat, unreal.MaterialExpressionScalarParameter, parameter_name=n, default_value=v)
cu, st, sm = scalar('Cumulus', 0.55), scalar('Stratus', 0.0), scalar('Storm', 0.0)
dens = scalar('Density', 0.02)
cu2, st2, sm2, dens2 = scalar('Cumulus2', 0.55), scalar('Stratus2', 0.0), scalar('Storm2', 0.0), scalar('Density2', 0.02)
# Far past the lake by default: the whole sky is the second weather (the same as the first).
front = node(mat, unreal.MaterialExpressionVectorParameter, parameter_name='Front', default_value=unreal.LinearColor(1, 0, 10000, 10))
wind = node(mat, unreal.MaterialExpressionVectorParameter, parameter_name='WindOffset', default_value=unreal.LinearColor(0, 0, 0, 0))

# Height in the layer: the normalised altitude times the layer's thickness (km), set by ALakeWeather.
height = custom(mat, 'return N * 9.0;', [('N', cloud, 'NormAltitudeInLayer')])
density = custom(mat, DENSITY, [('P', where), ('H', height), ('Cu', cu), ('St', st), ('Sm', sm),
                                ('WO', wind, 'RGBA'), ('D', dens), ('Cu2', cu2), ('St2', st2), ('Sm2', sm2), ('D2', dens2),
                                ('Fr', front, 'RGBA'), ('Shape', shape), ('Erosion', erosion)])
albedo = custom(mat, ALBEDO, [('P', where), ('H', height), ('St', st), ('Sm', sm), ('St2', st2), ('Sm2', sm2), ('Fr', front, 'RGBA')],
                unreal.CustomMaterialOutputType.CMOT_FLOAT3)
flash = node(mat, unreal.MaterialExpressionVectorParameter, parameter_name='Flash', default_value=unreal.LinearColor(0, 0, 0, 0))
glow = scalar('NightGlow', 0.0)
emissive = custom(mat, EMISSIVE, [('P', where), ('H', height), ('Dn', density), ('Fl', flash, 'RGBA'), ('Ng', glow)],
                  unreal.CustomMaterialOutputType.CMOT_FLOAT3)
lib.connect_material_property(emissive, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
# Extinction (grey) = density; albedo is the share of it scattered back out.
lib.connect_material_property(albedo, '', unreal.MaterialProperty.MP_BASE_COLOR)
lib.connect_material_property(density, '', unreal.MaterialProperty.MP_SUBSURFACE_COLOR)

# Light in the cloud: a forward-scattering lobe (silver linings towards the sun) blended with a
# softer backward one, and the multiple scattering that makes thick clouds glow inside.
advanced = node(mat, unreal.MaterialExpressionVolumetricAdvancedMaterialOutput)
for prop, value in (('const_phase_g', 0.75), ('const_phase_g2', -0.2), ('const_phase_blend', 0.45),
                    ('multi_scattering_approximation_octave_count', 2),
                    ('const_multi_scattering_contribution', 0.55), ('const_multi_scattering_occlusion', 0.35),
                    ('const_multi_scattering_eccentricity', 0.6), ('ground_contribution', True)):
    try:
        advanced.set_editor_property(prop, value)
    except Exception as e:
        unreal.log_warning('QUIET_LAKE_CLOUDS cannot set %s: %s' % (prop, e))

lib.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(PATH)
unreal.log('QUIET_LAKE_CLOUDS_READY')
