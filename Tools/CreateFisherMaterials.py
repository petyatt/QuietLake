"""Flat-colour materials for the angler's clothes: boots, canvas trousers, knitted sweater, jacket,
skin and the cap. Default Lit, roughness only — cheap, and they read at the game's distances.

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript \\
        -script=C:/Fish/Tools/CreateFisherMaterials.py
"""
import unreal

lib = unreal.MaterialEditingLibrary
MATS = {
    # name: (colour RGB, roughness)
    'M_Fisher_Boots': ((0.045, 0.050, 0.045), 0.45),     # dark rubber
    'M_Fisher_Pants': ((0.115, 0.120, 0.135), 0.85),     # faded dark canvas
    'M_Fisher_Sweater': ((0.135, 0.155, 0.100), 0.92),   # dull green knit
    'M_Fisher_Jacket': ((0.135, 0.115, 0.072), 0.80),    # worn olive canvas
    'M_Fisher_Skin': ((0.42, 0.29, 0.20), 0.65),
    'M_Fisher_Hat': ((0.075, 0.070, 0.055), 0.90),
}


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for key, value in props.items():
        n.set_editor_property(key, value)
    return n


for name, (rgb, rough) in MATS.items():
    path = '/Game/Characters/Fisher/' + name
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, '/Game/Characters/Fisher', unreal.Material, unreal.MaterialFactoryNew())
    colour = node(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(rgb[0], rgb[1], rgb[2], 1.0))
    lib.connect_material_property(colour, '', unreal.MaterialProperty.MP_BASE_COLOR)
    rough_node = node(mat, unreal.MaterialExpressionConstant, r=rough)
    lib.connect_material_property(rough_node, '', unreal.MaterialProperty.MP_ROUGHNESS)
    spec = node(mat, unreal.MaterialExpressionConstant, r=0.25)
    lib.connect_material_property(spec, '', unreal.MaterialProperty.MP_SPECULAR)
    lib.recompile_material(mat)
    if not unreal.EditorAssetLibrary.save_loaded_asset(mat):
        raise RuntimeError('cannot save ' + path)
    unreal.log('FISHER_MATERIAL ' + name)

unreal.log('FISHER_MATERIALS_READY')
