"""Give existing grass blades leaf transmission instead of opaque, dark back faces."""
import unreal

material = unreal.load_asset('/Game/Plants/M_GrassBlades')
if not material:
    raise RuntimeError('Missing grass material')
lib = unreal.MaterialEditingLibrary
colour = lib.get_material_property_input_node(material, unreal.MaterialProperty.MP_BASE_COLOR)
if not isinstance(colour, unreal.MaterialExpressionCustom):
    raise RuntimeError('Unexpected grass graph; left unchanged')
code = colour.get_editor_property('code')
if 'return C * 0.45 * lerp' not in code and 'return C * 0.60 * lerp' not in code:
    raise RuntimeError('Unexpected grass colour; left unchanged')
material.set_editor_property('two_sided', True)
material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
colour.set_editor_property('code', code.replace('return C * 0.45 * lerp', 'return C * 0.60 * lerp'))
if not lib.connect_material_property(colour, '', unreal.MaterialProperty.MP_SUBSURFACE_COLOR):
    raise RuntimeError('Cannot connect leaf transmission')
lib.recompile_material(material)
if not unreal.EditorAssetLibrary.save_loaded_asset(material):
    raise RuntimeError('Could not save grass material')
unreal.log('QUIET_LAKE_BANK_GRASS: two-sided foliage, leaf transmission, albedo factor 0.60, no emission')
