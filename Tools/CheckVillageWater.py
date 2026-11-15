"""Checks the saved lake map: every Water body draws a MI_Village* instance over M_VillageWater with
the live ripple inputs (Ripples, RippleWindow) and a working ripple strength.

Run with UnrealEditor-Cmd -run=pythonscript -script=C:/Fish/Tools/CheckVillageWater.py
"""
import unreal

lib = unreal.MaterialEditingLibrary
world = unreal.EditorLoadingAndSavingUtils.load_map('/Game/Maps/Lake')
assert world, 'Lake map must load'
count = 0
for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
    if not isinstance(actor, (unreal.WaterBodyLake, unreal.WaterBodyRiver)):
        continue
    body = actor.get_water_body_component()
    mat = body.get_water_material()
    name = actor.get_name()
    assert mat.get_path_name().startswith('/Game/Water/MI_Village'), name + ' still uses ' + mat.get_path_name()
    assert mat.get_base_material().get_path_name().startswith('/Game/Water/M_VillageWater'), name + ': not on M_VillageWater'
    assert 'Ripples' in [str(n) for n in lib.get_texture_parameter_names(mat)], name + ': ripple texture missing'
    assert 'RippleWindow' in [str(n) for n in lib.get_vector_parameter_names(mat)], name + ': ripple window missing'
    assert lib.get_material_instance_scalar_parameter_value(mat, 'LakeRippleStrength') > 1, name + ': ripples too weak'
    count += 1
assert count == 4, 'Expected the lake and three streams, found %d' % count
unreal.log('VILLAGE_WATER_CHECK passed: %d water bodies on M_VillageWater with live ripples' % count)
