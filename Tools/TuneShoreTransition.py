"""Soften the wet shoreline and remove square macro-colour steps in the existing terrain."""
import unreal
lib = unreal.MaterialEditingLibrary
material = unreal.load_asset('/Game/Materials/M_TerrainScanned')
if not material:
    raise RuntimeError('Missing terrain material')
colour = lib.get_material_property_input_node(material, unreal.MaterialProperty.MP_BASE_COLOR)
if not isinstance(colour, unreal.MaterialExpressionCustom):
    raise RuntimeError('Unexpected colour graph; left unchanged')
inputs = lib.get_inputs_for_material_expression(material, colour)
weights = next((n for n in inputs if isinstance(n, unreal.MaterialExpressionCustom) and 'float wet =' in n.get_editor_property('code')), None)
if weights is None:
    raise RuntimeError('Unexpected weight graph; left unchanged')
old_wet = 'float wet = 1 - saturate((P.z + 21) / 9);'
new_wet = 'float wet = 1 - smoothstep(-23.0, 16.0, P.z + (n - 0.5) * 14.0);'
old_macro = 'float macro = lerp(0.85, 1.12, frac(sin(dot(floor(P.xy / 900), float2(12.9898, 78.233))) * 43758.5453));'
new_macro = 'float2 cell = floor(P.xy / 900), blend = frac(P.xy / 900);\nblend = blend * blend * (3 - 2 * blend);\nfloat4 h = frac(sin(float4(dot(cell, float2(12.9898, 78.233)), dot(cell + float2(1, 0), float2(12.9898, 78.233)), dot(cell + float2(0, 1), float2(12.9898, 78.233)), dot(cell + 1, float2(12.9898, 78.233)))) * 43758.5453);\nfloat macro = lerp(0.85, 1.12, lerp(lerp(h.x, h.y, blend.x), lerp(h.z, h.w, blend.x), blend.y));'
weight_code = weights.get_editor_property('code')
colour_code = colour.get_editor_property('code')
if old_wet not in weight_code and new_wet not in weight_code:
    raise RuntimeError('Unexpected wetness expression; left unchanged')
if old_macro not in colour_code and new_macro not in colour_code:
    raise RuntimeError('Unexpected macro expression; left unchanged')
weights.set_editor_property('code', weight_code.replace(old_wet, new_wet))
colour.set_editor_property('code', colour_code.replace(old_macro, new_macro))
lib.recompile_material(material)
if not unreal.EditorAssetLibrary.save_loaded_asset(material):
    raise RuntimeError('Could not save terrain material')
unreal.log('QUIET_LAKE_SHORE_MATERIAL: smooth wet bank and continuous macro colour saved')
