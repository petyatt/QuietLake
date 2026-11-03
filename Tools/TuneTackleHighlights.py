"""Update only the float paint emission in existing assets; no mesh reimport."""
import unreal

material = unreal.load_asset('/Game/Tackle/M_Tackle')
if not material:
    raise RuntimeError('M_Tackle is missing')
node = unreal.MaterialEditingLibrary.get_material_property_input_node(
    material, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
if not isinstance(node, unreal.MaterialExpressionCustom):
    raise RuntimeError('Unexpected tackle emission graph; left unchanged')
code = node.get_editor_property('code')
old, new = 'return V * paint * 12;', 'return V * paint * 1.5;'
if old not in code and new not in code:
    raise RuntimeError('Unexpected paint expression; left unchanged')
node.set_editor_property('code', code.replace(old, new))
unreal.MaterialEditingLibrary.recompile_material(material)
if not unreal.EditorAssetLibrary.save_loaded_asset(material):
    raise RuntimeError('Could not save M_Tackle')
unreal.log('QUIET_LAKE_COLOR: fluorescent paint emission 1.5, mesh assets unchanged')
