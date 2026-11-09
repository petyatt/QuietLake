"""The look of a fish under water, shared by ImportMeshyFish.py and TuneFishMaterials.py.

A real fish is hard to see from above: its back is dark and dull, the colours go olive in the
peaty water, and the scales do not flash in the sun. Below the lake surface (world Z -25, as
LakeFish::SurfaceZ) the material darkens the colour (the back most), takes some of it away, and
dulls the sheen; and it takes off the sunlight the water swallows on its way down to the fish
(the water material only dims the way from the fish to the eye, so a fish 40 cm down was lit as
brightly as one at the top). Out of the water (on the line, in the hand) the fish shows its own
colours.
"""
import unreal

lib = unreal.MaterialEditingLibrary
MARK = 'QL_UnderwaterLook'

UNDER = 'saturate((-25.0 - P.z) / 3.0)'   # 0 above the lake surface, 1 from 3 cm below it

COLOUR = """
float under = %s;
float top = saturate(N.z);
float3 dull = C * lerp(0.34, 0.20, top);
float l = dot(dull, float3(0.3, 0.59, 0.11));
dull = lerp(dull, l * float3(0.92, 1.0, 0.72), 0.45);
// Incident light fades with depth. M_LakeSurface attenuates the separate path to the viewer.
float3 light = exp(-float3(0.042, 0.034, 0.060) * max(-25.0 - P.z, 0.0));
return lerp(C, dull * light, under);
""" % UNDER

ROUGH = 'return lerp(R, max(R, 0.78), %s);' % UNDER
SPEC = 'return lerp(0.55, 0.04, %s);' % UNDER


def _node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def _connect(src, out, dst, inp):
    if not lib.connect_material_expressions(src, out, dst, inp):
        raise RuntimeError('Material link failed: %s.%s -> %s.%s' % (src.get_name(), out, dst.get_name(), inp))


def _custom(mat, code, inputs, output):
    n = _node(mat, unreal.MaterialExpressionCustom, code=code, output_type=output, description=MARK)
    items = []
    for name, _, _ in inputs:
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        items.append(item)
    n.set_editor_property('inputs', items)
    for name, src, out in inputs:
        _connect(src, out, n, name)
    return n


def has_look(mat):
    node = lib.get_material_property_input_node(mat, unreal.MaterialProperty.MP_BASE_COLOR)
    return node is not None and isinstance(node, unreal.MaterialExpressionCustom) and node.get_editor_property('description') == MARK


def refresh(mat):
    """Puts the current code into a material that has the look already. True if it had it."""
    found = False
    for prop, code in ((unreal.MaterialProperty.MP_BASE_COLOR, COLOUR), (unreal.MaterialProperty.MP_ROUGHNESS, ROUGH),
                       (unreal.MaterialProperty.MP_SPECULAR, SPEC)):
        node = lib.get_material_property_input_node(mat, prop)
        if node is not None and isinstance(node, unreal.MaterialExpressionCustom) and node.get_editor_property('description') == MARK:
            node.set_editor_property('code', code)
            found = True
    return found


def apply(mat, colour, colour_out, rough, rough_out):
    """Wires the underwater look between the textures and the material outputs (colour, roughness,
    specular). colour / rough: the nodes feeding them, with their output names ('RGB', 'R')."""
    position = _node(mat, unreal.MaterialExpressionWorldPosition)
    normal = _node(mat, unreal.MaterialExpressionVertexNormalWS)
    c = _custom(mat, COLOUR, [('C', colour, colour_out), ('N', normal, ''), ('P', position, '')], unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    lib.connect_material_property(c, '', unreal.MaterialProperty.MP_BASE_COLOR)
    r = _custom(mat, ROUGH, [('R', rough, rough_out), ('P', position, '')], unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    lib.connect_material_property(r, '', unreal.MaterialProperty.MP_ROUGHNESS)
    s = _custom(mat, SPEC, [('P', position, '')], unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    lib.connect_material_property(s, '', unreal.MaterialProperty.MP_SPECULAR)
