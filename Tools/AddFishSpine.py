"""Bend the fish models along a hooked fish's spine (FFishSpine in Source/QuietLake/LakeFishSpine.*).

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/AddFishSpine.py

The swim wave of M_Meshy_<Species> (a Custom node in World Position Offset, reading the wave coordinate
from the vertex red channel) gets the spine: Custom Primitive Data 5..12 are the directions (yaw,
pitch) of the spine's segments 1..4 from the head's, in the model's own frame; segment 0 is the head.
Along the body the direction is blended between the segments' middles and the spine is integrated from
the snout, so the whole model, tail fin and all, curves smoothly. With all of them zero (a free fish)
the material does exactly what it did. Running it again replaces the spine node.
"""
import unreal

lib = unreal.MaterialEditingLibrary
SPECIES = ['Bleak', 'Bream', 'Crucian', 'Perch', 'Pike', 'Roach', 'Rudd']

# Added after each material's own swim wave (which ends by returning the sideways offset `side`).
# First the fins (marked in the vertex colours by Tools/MarkFishFins.py: green = 1 - fin share, blue =
# which fin), moved in the model's own frame: the pectorals paddle, the dorsal and the anal fins are
# raised or laid flat and flutter, the tail fin flutters (Custom Primitive Data 13 paddle 0..1, 14
# spread -1 laid flat .. 1 raised, 15 rate, 16 = 1 shows the fins blown out for checking the marks).
# Then the spine bends the whole model.
SPINE = """
float fw = 1 - V.g;
float3 fin = 0;
if (fw > 0.001)
{
    float kind = V.b;
    float t = T * FR;
    float sideSign = LP.y >= 0 ? 1.0 : -1.0;
    float up = LP.z >= 0 ? 1.0 : -1.0;
    float k = fw * Len;
    if (kind > 0.37 && kind <= 0.62)
        fin += k * float3(0.045 * sin(t), sideSign * 0.03 * FS, 0.04 * cos(t)) * (0.3 + FP);
    else if (kind > 0.12 && kind <= 0.9)
        fin += k * float3(-0.05 * FS, 0.03 * FP * sin(t * 1.7 + LP.x / Len * 9.0), 0.045 * up * FS);
    else if (kind <= 0.12)
        fin += k * float3(0, 0.035 * FP * sin(t * 1.3 + 1.2), 0);
    if (FD > 0.5) fin += fw * float3(0, LP.y, LP.z) * 1.5;
}
float3 LQ = LP + fin;
float3 off = float3(0, side, 0) + fin;
float ya[5] = { 0, Y1, Y2, Y3, Y4 };
float pa[5] = { 0, P1, P2, P3, P4 };
float bent = abs(Y1) + abs(Y2) + abs(Y3) + abs(Y4) + abs(P1) + abs(P2) + abs(P3) + abs(P4);
if (bent > 0.0001)
{
    float segLen = Len / 5.0;
    float x = saturate(V.r) * Len;
    float dx = x / 8.0;
    float3 S = 0;
    for (int i = 0; i < 8; i++)
    {
        float s = clamp(((i + 0.5) * dx) / segLen - 0.5, 0.0, 4.0);
        int i0 = min((int)floor(s), 3);
        float f = s - i0;
        float yy = lerp(ya[i0], ya[i0 + 1], f);
        float pp = lerp(pa[i0], pa[i0 + 1], f);
        S += dx * float3(cos(yy) * cos(pp), sin(yy) * cos(pp), sin(pp));
    }
    float s = clamp(x / segLen - 0.5, 0.0, 4.0);
    int i0 = min((int)floor(s), 3);
    float f = s - i0;
    float yaw = lerp(ya[i0], ya[i0 + 1], f);
    float pit = lerp(pa[i0], pa[i0 + 1], f);
    // The cross-section turns with the spine: pitch about Y, then yaw about Z.
    float cy = LQ.y + side, cz = LQ.z;
    float rx = -cz * sin(pit);
    float rz = cz * cos(pit);
    float3 r = float3(rx * cos(yaw) - cy * sin(yaw), rx * sin(yaw) + cy * cos(yaw), rz);
    off = S + r + float3(fin.x, 0, 0) - float3(x, LP.y, LP.z);
}
return off;
"""
MARK = '// spine'


def wave_part(code):
    """The material's own swim wave, up to (not including) its return; an earlier spine is dropped."""
    if MARK in code:
        code = code[:code.index(MARK)]
    lines = [l for l in code.split(chr(10)) if 'return' not in l]
    return chr(10).join(lines).rstrip() + chr(10) + MARK + chr(10)



def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def prim_data(mat, name, index):
    p = node(mat, unreal.MaterialExpressionScalarParameter, parameter_name=name, default_value=0.0)
    p.set_editor_property('use_custom_primitive_data', True)
    p.set_editor_property('primitive_data_index', index)
    return p


def local_position(mat):
    cls = getattr(unreal, 'MaterialExpressionLocalPosition', None)
    if cls:
        return node(mat, cls), ''
    # (Older engines: the world position without offsets, brought into the local frame.)
    world = node(mat, unreal.MaterialExpressionWorldPosition)
    world.set_editor_property('world_position_shader_offset', unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    to_local = node(mat, unreal.MaterialExpressionTransformPosition,
                    transform_source_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_WORLD,
                    transform_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_LOCAL)
    lib.connect_material_expressions(world, '', to_local, '')
    return to_local, ''


for species in SPECIES:
    path = '/Game/Fish/M_Meshy_' + species
    mat = unreal.load_asset(path)
    if not mat:
        unreal.log_warning('FISH_SPINE no material ' + path)
        continue
    to_world = lib.get_material_property_input_node(mat, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    old = lib.get_inputs_for_material_expression(mat, to_world)[0]
    names = [str(i.get_editor_property('input_name')) for i in old.get_editor_property('inputs')]
    code = wave_part(old.get_editor_property('code')) + SPINE
    sources = lib.get_inputs_for_material_expression(mat, old)
    have = dict(zip(names, sources))
    # Earlier runs left their own spine inputs: reuse what is there, add the rest.
    spine_names = ['Y1', 'P1', 'Y2', 'P2', 'Y3', 'P3', 'Y4', 'P4']
    for k, name in enumerate(spine_names):
        if name not in have or have[name] is None:
            have[name] = prim_data(mat, 'Spine' + name, 5 + k)
    if 'LP' not in have or have['LP'] is None:
        have['LP'] = local_position(mat)[0]
    fin_names = [('FP', 13), ('FS', 14), ('FR', 15), ('FD', 16)]
    for name, index in fin_names:
        if name not in have or have[name] is None:
            have[name] = prim_data(mat, 'Fin' + name, index)
    order = ['V', 'T', 'R', 'A', 'Ph', 'Len', 'C', 'LP'] + spine_names + [n for n, _ in fin_names]
    missing = [n for n in order if n not in have or have[n] is None]
    assert not missing, (species, missing)
    spine = node(mat, unreal.MaterialExpressionCustom, code=code, output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                 description='Swim wave and spine')
    items = []
    for name in order:
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        items.append(item)
    spine.set_editor_property('inputs', items)
    for name in order:
        lib.connect_material_expressions(have[name], '', spine, name)
    lib.connect_material_expressions(spine, '', to_world, '')
    lib.delete_material_expression(mat, old)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path)
    unreal.log('FISH_SPINE ' + species + ' inputs ' + ','.join(order))
