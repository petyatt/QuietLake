"""Assign tuned native Water surfaces without rebuilding terrain; create them first.

Run with UnrealEditor-Cmd -run=pythonscript -script=C:/Fish/Tools/ApplyWaterMaterials.py
"""
import unreal

ROOT = '/Game/Water/'
MAP = '/Game/Maps/Lake'


def material(relative):
    asset = unreal.load_asset(ROOT + relative)
    if not isinstance(asset, unreal.MaterialInterface):
        raise RuntimeError('Missing water material: ' + relative)
    return asset


surfaces = {kind: material('MI_Village' + kind) for kind in ('Lake', 'River')}
lods = {kind: material('MI_Village' + kind + 'LOD') for kind in surfaces}
transition = material('MI_VillageRiverToLake')
world = unreal.EditorLoadingAndSavingUtils.load_map(MAP)
if not world:
    raise RuntimeError('Cannot load ' + MAP)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
count = 0
with unreal.ScopedEditorTransaction('Use native Water materials'):
    for actor in actors:
        if not isinstance(actor, (unreal.WaterBodyLake, unreal.WaterBodyRiver)):
            continue
        body = actor.get_water_body_component()
        kind = 'River' if isinstance(actor, unreal.WaterBodyRiver) else 'Lake'
        actor.modify()
        body.modify()
        body.set_water_material(surfaces[kind])
        body.set_water_static_mesh_material(lods[kind])
        if kind == 'River':
            body.set_lake_transition_material(transition)
        if body.get_water_material() != surfaces[kind]:
            raise RuntimeError('Material assignment failed: ' + actor.get_name())
        unreal.log('QUIET_LAKE_NATIVE_WATER ' + actor.get_name() + ': ' + surfaces[kind].get_path_name())
        count += 1
if not count:
    raise RuntimeError('No lake or river actors found')
if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
    raise RuntimeError('Cannot save ' + MAP)
unreal.log('QUIET_LAKE_NATIVE_WATER saved %d water bodies' % count)
