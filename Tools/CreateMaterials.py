import unreal

assets = unreal.AssetToolsHelpers.get_asset_tools()
for name, rgb, metallic, roughness in [
    ('M_Bank', (0.16, 0.23, 0.075), 0.0, 0.95),
    ('M_Water', (0.018, 0.17, 0.24), 0.5, 0.18),
    ('M_Wood', (0.18, 0.085, 0.035), 0.0, 0.85),
]:
    path = '/Game/Materials/' + name
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        continue
    material = assets.create_asset(name, '/Game/Materials', unreal.Material, unreal.MaterialFactoryNew())
    color = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant3Vector)
    color.set_editor_property('constant', unreal.LinearColor(*rgb, 1.0))
    unreal.MaterialEditingLibrary.connect_material_property(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
    for value, prop in [(metallic, unreal.MaterialProperty.MP_METALLIC), (roughness, unreal.MaterialProperty.MP_ROUGHNESS)]:
        scalar = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant)
        scalar.set_editor_property('r', value)
        unreal.MaterialEditingLibrary.connect_material_property(scalar, '', prop)
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path)
unreal.log('QUIET_LAKE_MATERIALS_READY')
