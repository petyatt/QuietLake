"""Village pond look for the engine Water: project material instances over the engine ones.

Only parameters change (colour, murkiness). The lake, the streams and the river-to-lake transition
sit on M_VillageWater (the engine Water graph plus the game's ripples, CreateVillageWater.py) with
every setting of their engine instance copied over; the far LODs stay on the engine instances.
Run with UnrealEditor-Cmd -run=pythonscript -script=C:/Fish/Tools/TuneVillageWater.py after
CreateVillageWater.py, then ApplyWaterMaterials.py assigns the instances to the lake and the streams.
Absorption: distance light of each colour travels (bigger = clearer), A scales all three.
Scattering: suspended silt and algae; bigger = murkier, its colour tints the haze.
"""
import unreal

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
FOLDER = '/Game/Water'
ROOT = '/Water/Materials/WaterSurface/'
RIPPLE_BASE = unreal.load_asset(FOLDER + '/M_VillageWater')
assert RIPPLE_BASE, 'Run CreateVillageWater.py first'

# Humic pond water: green and red carry about as far, blue dies first - a brownish olive haze.
LAKE = {
    'Absorption': (9.5, 11.5, 5.0, 8.0),
    'Scattering': (0.7, 0.66, 0.48, 0.5),
    'Water Albedo': (0.36, 0.35, 0.3, 0.5),
}
# Streams run shallow over the clay: a little clearer and browner.
RIVER = {
    'Absorption': (14.0, 15.5, 6.5, 8.0),
    'Scattering': (0.5, 0.45, 0.37, 0.5),
    'Water Albedo': (0.4, 0.38, 0.3, 0.5),
}
# How strongly the ALakeRipples height field bends the Water normal. In a calm the lake is a mirror
# (ULakeWaves fades the engine's own normals with the wind) and every ring shows: at 3 a bite's rings
# are bold bands metres wide, at 8 and more they are thick dark stripes. In a wind the rings are
# mostly lost in the ripple, as on a real lake.
RIPPLE_STRENGTH = 1.5
# The lake's fine ripple is M_VillageWater's own, drifting downwind (ULakeWaves): the engine's, which
# pans its normal map four ways whatever the wind, is turned off. The streams keep the engine's.
LAKE_SCALARS = {
    'Default Near Normal Strength': 0.02,
    'Default Distant Normal Strength': 0.02,
    'Default Distant Normal StrengthB': 0.02,
}
# Name: (engine instance, look, with the live ripples).
VARIANTS = {
    'MI_VillageLake': ('Water_Material_Lake', LAKE, True),
    'MI_VillageLakeLOD': ('LODs/Water_Material_Lake_LOD', LAKE, False),
    'MI_VillageRiver': ('Water_Material_River', RIVER, True),
    'MI_VillageRiverLOD': ('LODs/Water_Material_River_LOD', RIVER, False),
    'MI_VillageRiverToLake': ('Transitions/Water_Material_River_To_Lake_Transition', RIVER, True),
}


def copy_settings(source, instance):
    """Every parameter of the engine instance, resolved through its parents, as overrides."""
    for kind in ('scalar', 'vector', 'texture', 'static_switch'):
        for param in getattr(lib, 'get_' + kind + '_parameter_names')(source):
            value = getattr(lib, 'get_material_instance_' + kind + '_parameter_value')(source, param)
            if value is not None:
                getattr(lib, 'set_material_instance_' + kind + '_parameter_value')(instance, param, value)


for name, (relative, values, ripples) in VARIANTS.items():
    source = unreal.load_asset(ROOT + relative)
    assert source, 'Missing engine water material ' + relative
    path = FOLDER + '/' + name
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        instance = unreal.load_asset(path)
    else:
        instance = assets.create_asset(name, FOLDER, unreal.MaterialInstanceConstant,
                                       unreal.MaterialInstanceConstantFactoryNew())
    assert isinstance(instance, unreal.MaterialInstanceConstant), path
    if ripples:
        lib.set_material_instance_parent(instance, RIPPLE_BASE)
        copy_settings(source, instance)
        lib.set_material_instance_scalar_parameter_value(instance, 'LakeRippleStrength', RIPPLE_STRENGTH)
        if name == 'MI_VillageLake':
            for param, value in LAKE_SCALARS.items():
                lib.set_material_instance_scalar_parameter_value(instance, param, value)
    else:
        lib.set_material_instance_parent(instance, source)
    known = [str(n) for n in lib.get_vector_parameter_names(source)]
    for param, value in values.items():
        if param not in known:
            unreal.log_warning('QUIET_LAKE_WATER_LOOK %s has no %s' % (name, param))
            continue
        lib.set_material_instance_vector_parameter_value(instance, param, unreal.LinearColor(*value))
    lib.update_material_instance(instance)
    assert unreal.EditorAssetLibrary.save_loaded_asset(instance), 'Cannot save ' + path
    unreal.log('QUIET_LAKE_WATER_LOOK %s over %s (settings of %s)' % (
        path, instance.get_editor_property('parent').get_path_name(), source.get_path_name()))
