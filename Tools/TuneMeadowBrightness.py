"""Tone down the meadow layer in the existing terrain material."""
import unreal

material = unreal.load_asset('/Game/Materials/M_TerrainScanned')
if not material:
    raise RuntimeError('M_TerrainScanned is missing')
node = unreal.MaterialEditingLibrary.get_material_property_input_node(
    material, unreal.MaterialProperty.MP_BASE_COLOR)
if not isinstance(node, unreal.MaterialExpressionCustom):
    raise RuntimeError('Unexpected terrain color graph; left unchanged')
code = node.get_editor_property('code')
old = 'float3 c = G * g + F * f + M * m;'
new = 'float3 c = G * (g * 0.78) + F * f + M * m;'
if old not in code and new not in code:
    raise RuntimeError('Unexpected terrain layers; left unchanged')
node.set_editor_property('code', code.replace(old, new))
unreal.MaterialEditingLibrary.recompile_material(material)
if not unreal.EditorAssetLibrary.save_loaded_asset(material):
    raise RuntimeError('Could not save M_TerrainScanned')
unreal.log('QUIET_LAKE_COLOR: meadow albedo scale 0.78; other layers and lighting unchanged')
