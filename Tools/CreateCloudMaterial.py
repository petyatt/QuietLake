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
// Position in km and height above the layer bottom in km.
float3 pk = P / 100000.0;
float3 w = float3(WO.x, WO.y, 0);
float h = H;

// The front: the old weather clouds thin out across its whole width, the coming ones arrive with a
// sharp leading edge; where they bring rain a line of storm towers stands on that edge.
float fx = Fr.z - dot(pk.xy, Fr.xy);
float fOld = 1.0 - smoothstep(-Fr.w, Fr.w, fx);
float fNew = smoothstep(-0.15 * Fr.w, 0.15 * Fr.w, fx + 0.3 * Fr.w);
float wall = fNew * (1.0 - fNew) * 4.0;
float kCu = max(Cu * fOld, Cu2 * fNew), kSt = max(St * fOld, St2 * fNew);
float kSm = max(Sm * fOld, Sm2 * saturate(fNew + wall * 0.6));
float kD = lerp(D, D2, fNew);

// Where cloud fields gather: broad patches (tens of km), drifting with the wind.
float patch = Shape.SampleLevel(ShapeSampler, float3((pk.xy + w.xy * 0.6) / 26.0, 0.31 + WO.z * 0.0015), 0).r;
float patch2 = Shape.SampleLevel(ShapeSampler, float3((pk.xy + w.xy * 0.5) / 9.0, 0.71), 0).r;

// The billows: a 3D Perlin-Worley, the shape every cloud kind is carved from.
float4 s = Shape.SampleLevel(ShapeSampler, float3((pk.xy + w.xy * 1.2) / 3.6, h / 3.0 + WO.z * 0.018), 0);
float fbm = s.g * 0.82 + s.b * 0.13 + s.a * 0.05;
float billow = saturate((s.r - (fbm - 1.0)) / (2.0 - fbm));

// A cumulus: a 2D field says where clouds gather, and the heap is a dome whose footprint shrinks
// with height (sqrt of one minus height squared), so it is a hemisphere with a flat base, not a
// column or a pancake. A 3D Perlin-Worley carves the cauliflower surface: the density is that noise
// remapped by the dome's coverage, so the billows themselves make the silhouette.
float4 cn = Shape.SampleLevel(ShapeSampler, float3((pk.xy + w.xy) / 5.0, 0.53 + WO.z * 0.002), 0);
float cells = saturate(cn.r * 0.65 + cn.g * 0.35 + (patch - 0.5) * 0.50);
float cu = saturate(kCu * 0.90);
float th = lerp(0.62, 0.20, cu);
float cov = smoothstep(th, th + 0.16, cells);
float baseCu = 0.10 + 0.05 * (cn.b - 0.5) + 0.02 * (patch - 0.5);
float topCu = baseCu + 0.75 + 1.30 * cov + 0.35 * patch2;
float hh = saturate((h - baseCu) / max(topCu - baseCu, 0.2));
float dome = sqrt(saturate(1.0 - hh * hh));
float c = saturate(cov * dome * 1.1);
float dcu = saturate((billow - (1.0 - c)) / max(c, 0.05));
// A finer noise frays the surface just enough to read as cloud, not as a smooth ball.
float4 bs = Shape.SampleLevel(ShapeSampler, float3((pk.xy + w.xy * 1.5) / 1.6, h / 1.15 + WO.z * 0.04), 0);
float det = bs.g * 0.7 + bs.b * 0.3;
dcu = saturate((dcu - (1.0 - det) * 0.12) / 0.88);
dcu *= smoothstep(baseCu, baseCu + 0.10, h);      // flat base
dcu = saturate(dcu * 1.8);

// Stratus: a closed deck with a lumpy underside. Thin spots let more light through, so the grey
// ceiling has bright and dark patches instead of being one flat lid.
float st = saturate(kSt);
float4 s2 = Shape.SampleLevel(ShapeSampler, float3((pk.xy + w.xy * 1.1) / 5.5, h / 3.0 + WO.z * 0.008), 0);
float fbm2 = s2.g * 0.82 + s2.b * 0.13 + s2.a * 0.05;
float shape2 = saturate((s2.r - (fbm2 - 1.0)) / (2.0 - fbm2));
float baseSt = 0.42 + 0.18 * (s2.g - 0.5) + 0.10 * (patch2 - 0.5);
float topSt = baseSt + 0.45 + 0.35 * patch2;
float vertSt = smoothstep(baseSt, baseSt + 0.15, h) * (1.0 - smoothstep(topSt - 0.25, topSt, h));
float dst = st * vertSt * (0.30 + 0.85 * shape2);

// Storm: towers from just over the bottom up to ~7 km, an anvil spreading under the top; denser
// than the rest, so the bases are dark and the anvil thins out at its edge.
float sm = saturate(kSm * (0.45 + 0.9 * patch2));
float baseSm = 0.05 + 0.10 * (cn.r - 0.5) + 0.04 * (patch2 - 0.5);
float psm = smoothstep(baseSm, baseSm + 0.24, h) * (1.0 - smoothstep(6.4, 7.8, h));
float anvil = smoothstep(4.8, 6.2, h) * (1.0 - smoothstep(6.4, 7.8, h));
float tower = saturate(1.35 - h / 8.0);
float fSm = sm * psm * lerp(tower, 1.0, anvil) * 0.8;
float dsm = saturate((lerp(billow, 1.0, 0.25) - (1.0 - fSm)) / max(fSm, 0.03)) * 1.8;

float d = max(max(dcu, dst), dsm);
if (d <= 0.0) return 0.0;

// Fray the edges: wispy at the bases, billowy crowns. The erosion is large-scale so the edges soften
// into wisps instead of graining the surface.
float3 eu = (pk + w * 1.4) / 2.2 + float3(0, 0, WO.z * 0.03);
float3 e3 = Erosion.SampleLevel(ErosionSampler, eu, 1).rgb;
float er = e3.r * 0.85 + e3.g * 0.15;
float erode = lerp(0.02, 0.06, saturate(h * 0.9));
d = saturate((d - er * erode) / (1.0 - erode));
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
float3 c = lerp(float3(0.95, 0.95, 0.96), float3(0.72, 0.74, 0.78), saturate(kSt * 0.5));
float3 storm = lerp(float3(0.10, 0.12, 0.16), float3(0.46, 0.49, 0.54), saturate((H - 0.3) / 4.0));
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
for prop, value in (('const_phase_g', 0.78), ('const_phase_g2', -0.25), ('const_phase_blend', 0.5),
                    ('multi_scattering_approximation_octave_count', 2),
                    ('const_multi_scattering_contribution', 0.42), ('const_multi_scattering_occlusion', 0.55),
                    ('const_multi_scattering_eccentricity', 0.6), ('ground_contribution', False)):
    try:
        advanced.set_editor_property(prop, value)
    except Exception as e:
        unreal.log_warning('QUIET_LAKE_CLOUDS cannot set %s: %s' % (prop, e))

lib.recompile_material(mat)
unreal.EditorAssetLibrary.save_asset(PATH)
unreal.log('QUIET_LAKE_CLOUDS_READY')
