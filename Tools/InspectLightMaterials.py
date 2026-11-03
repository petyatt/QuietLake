"""Log the actual saved shaders used by the grass, water and float (read only)."""
import unreal

for path in ('/Game/Plants/M_GrassBlades', '/Game/Materials/M_TerrainScanned', '/Game/Tackle/M_Tackle', '/Game/Materials/M_LakeSurface'):
    mat = unreal.load_asset(path)
    if not mat:
        raise RuntimeError('Missing material: ' + path)
    for prop in (unreal.MaterialProperty.MP_BASE_COLOR, unreal.MaterialProperty.MP_EMISSIVE_COLOR,
                 unreal.MaterialProperty.MP_ROUGHNESS, unreal.MaterialProperty.MP_SPECULAR):
        node = unreal.MaterialEditingLibrary.get_material_property_input_node(mat, prop)
        value = 'unconnected'
        if isinstance(node, unreal.MaterialExpressionCustom):
            value = node.get_editor_property('code')
        elif isinstance(node, unreal.MaterialExpressionConstant):
            value = str(node.get_editor_property('r'))
        elif node:
            value = node.get_class().get_name()
        unreal.log('LIGHT_MATERIAL %s %s: %s' % (path, prop, value))
