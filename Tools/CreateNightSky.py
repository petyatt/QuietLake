"""Build the night-only sky background, with an angular moon and stable stars."""
import unreal

lib = unreal.MaterialEditingLibrary
path = '/Game/Weather/M_NightSky'
mat = unreal.load_asset(path)
if mat is None:
    mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        'M_NightSky', '/Game/Weather', unreal.Material, unreal.MaterialFactoryNew())
lib.delete_all_material_expressions(mat)
mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
mat.set_editor_property('two_sided', True)
mat.set_editor_property('is_sky', True)

def node(cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n

inputs = {
    'View': node(unreal.MaterialExpressionCameraVectorWS),
    'Atmosphere': node(unreal.MaterialExpressionSkyAtmosphereViewLuminance),
    'MoonDirection': node(unreal.MaterialExpressionVectorParameter, parameter_name='MoonDirection', default_value=unreal.LinearColor(0, 0, 1, 0)),
    'Visibility': node(unreal.MaterialExpressionScalarParameter, parameter_name='Visibility', default_value=0.0),
}
code = r'''
float3 d = normalize(-View);
float2 uv = float2(atan2(d.y,d.x)/6.2831853+0.5, acos(clamp(d.z,-1,1))/3.14159265);
float2 grid = uv * float2(900,450);
float2 cell = floor(grid);
float3 hash = frac(sin(float3(dot(cell,float2(127.1,311.7)),dot(cell,float2(269.5,183.3)),dot(cell,float2(419.2,371.9))))*43758.5453);
float exists = frac(sin(dot(cell,float2(93.7,287.3)))*15731.743);
float2 offset = frac(grid) - (0.08+0.84*hash.xy);
// Preserve flux for subpixel stars instead of expanding every dot to a bright pixel.
float radius = lerp(0.022,0.062,pow(hash.z,4));
float2 footprint = fwidth(grid);
float variance = radius*radius + dot(footprint,footprint)/12.0;
float star = exp(-dot(offset,offset)/(2*variance)) * radius*radius/variance;
star *= step(exists,0.055*sin(uv.y*3.14159265));
// Many faint stars, a small bright tail; positions and brightness are independent. Brighter than
// before: with the night exposure fixed dark, the stars must still read as points of light.
float brightness = lerp(0.0016,0.20,pow(hash.z,8));
float3 starColour = lerp(float3(0.82,0.88,1),float3(1,0.92,0.82),hash.y);
float3 m = normalize(MoonDirection);
float3 right = normalize(cross(float3(0,0,1),m)+float3(0.00001,0,0));
float3 up = cross(m,right);
// 0.8 degree diameter, slightly enlarged for the game camera.
float2 p = float2(dot(d,right),dot(d,up))/0.0069813;
float r = length(p);
float edge = max(fwidth(r),0.005);
float disk = (1-smoothstep(1-edge,1+edge,r))*step(0,dot(d,m));
float maria = exp(-dot(p-float2(-0.3,0.2),p-float2(-0.3,0.2))*7)
    + 0.6*exp(-dot(p-float2(0.25,0.4),p-float2(0.25,0.4))*16)
    + 0.45*exp(-dot(p-float2(0.3,-0.3),p-float2(0.3,-0.3))*22);
float texture = 0.94-0.34*maria + 0.025*sin(p.x*33)*sin(p.y*29);
float limb = 0.5+0.5*sqrt(saturate(1-r*r));
float horizon = smoothstep(0.015,0.18,d.z);
float glare = 1-0.85*exp(-max(0,1-dot(d,m))*150);
return Atmosphere + Visibility*horizon*(star*starColour*brightness*glare*(1-disk)
    + disk*float3(0.06,0.057,0.052)*texture*limb);
'''
custom = node(unreal.MaterialExpressionCustom, code=code, output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3)
pins = []
for name in inputs:
    pin = unreal.CustomInput()
    pin.set_editor_property('input_name', name)
    pins.append(pin)
custom.set_editor_property('inputs', pins)
for name, src in inputs.items():
    if not lib.connect_material_expressions(src, '', custom, name):
        raise RuntimeError('Cannot connect ' + name)
if not lib.connect_material_property(custom, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR):
    raise RuntimeError('Cannot connect sky output')
lib.recompile_material(mat)
if not unreal.EditorAssetLibrary.save_loaded_asset(mat):
    raise RuntimeError('Cannot save night sky')
unreal.log('QUIET_LAKE_NIGHT_SKY_MATERIAL_READY')
