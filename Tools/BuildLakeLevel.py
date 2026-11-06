"""Build the lake level /Game/Maps/Lake in the full editor (the water brushes carve the landscape on
the GPU, which the commandlet does not have), save it and quit:

    UnrealEditor.exe C:/Fish/QuietLake.uproject -ExecCmds="py C:/Fish/Tools/BuildLakeLevel.py"

(not -ExecutePythonScript: that quits the editor as soon as the script returns, before the frames run)

1. a new blank level (an existing /Game/Maps/Lake is replaced);
2. landscape layer infos LI_Meadow / LI_Forest / LI_Clay and the landscape from LakeMap with
   M_LakeLandscape (Tools/CreateTerrainMaterial.py builds it);
3. the engine Water lake along the waterline, carving its bowl, a Water river per stream and Water
   islands under the water that raise the old weedy flats back to ~1 m
   (ULakeLevelLibrary, Source/QuietLake/LakeLevel.cpp); the Water editor adds the brush manager and
   the water zone by itself;
4. a few seconds of editor frames for the brushes to render into the landscape, then save.
5. the carved heights baked to Content/Maps/LakeGround.bin for LakeMap::Ground (fish, tackle, tests).
Settings (environment): QUIETLAKE_LAKE_DEPTH (cm, default 600), QUIETLAKE_LAKE_RAMP (cm, default 6000);
the bed's profile across the ramp is the curve /Game/Maps/LakeBedProfile (ULakeLevelLibrary::CreateBedProfile).
"""
import os

import unreal

MAP = '/Game/Maps/Lake'
LAYERS = '/Game/Maps/LakeLayers'
DEPTH = float(os.environ.get('QUIETLAKE_LAKE_DEPTH', '600'))
RAMP = float(os.environ.get('QUIETLAKE_LAKE_RAMP', '6000'))
lib = unreal.LakeLevelLibrary
state = {'step': 0, 'frames': 0, 'handle': None, 'busy': False}


def log(text):
    unreal.log('QUIET_LAKE_LEVEL ' + text)


def tick(delta):
    # Long steps (the landscape import) show progress dialogs that pump editor frames: never run
    # a step inside another one.
    if state.get('busy'):
        return
    state['frames'] += 1
    step, frames = state['step'], state['frames']
    state['busy'] = True
    try:
        if step == 0 and frames > 30:
            # The whole landscape (1387 x 1135) is merged with the water brushes in one batch; the
            # default limit of 1024 skipped the merge and the bowl was never carved.
            unreal.SystemLibrary.execute_console_command(None, 'landscape.BatchedMerge.MaxResolutionPerRenderBatch 2048')
            if os.environ.get('QUIETLAKE_LEVEL_VERBOSE'):
                unreal.SystemLibrary.execute_console_command(None, 'log LogWaterEditor Verbose')
                unreal.SystemLibrary.execute_console_command(None, 'log LogLandscape Verbose')
            unreal.EditorAssetLibrary.make_directory('/Game/Maps')
            # Always from a blank map, saved over MAP at the end: rebuilding inside the old level left
            # the destroyed landscape in memory, and the new lake bound itself to it and carved nothing.
            # (new_level crashed when the folder was new.)
            # (The old map is the editor's startup map: leave it first, then delete it.)
            unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
            if unreal.EditorAssetLibrary.does_asset_exist(MAP) and not unreal.EditorAssetLibrary.delete_asset(MAP):
                raise RuntimeError('cannot replace ' + MAP)
            log('new blank map')
            state.update(step=1, frames=0)
        elif step == 1 and frames > 30:
            infos = [lib.create_layer_info(LAYERS, name) for name in ('Meadow', 'Forest', 'Clay')]
            for info in infos:
                unreal.EditorAssetLibrary.save_loaded_asset(info)
            material = unreal.load_asset('/Game/Materials/M_LakeLandscape')
            if not material:
                raise RuntimeError('M_LakeLandscape missing: run Tools/CreateTerrainMaterial.py first')
            lib.build_landscape(material, infos)
            state.update(step=2, frames=0)
        elif step == 2 and frames > 30:
            profile = lib.create_bed_profile('/Game/Maps', 'LakeBedProfile')
            unreal.EditorAssetLibrary.save_loaded_asset(profile)
            lib.spawn_lake(DEPTH, RAMP, 600, profile)
            lib.spawn_streams(60)
            lib.spawn_shallows(140, 500, 400, 100, 12)
            state.update(step=3, frames=0)
        elif step == 3 and frames in (120, 300, 480):
            # Ask the water brush manager to render the brushes again, then merge and measure.
            for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
                if actor.get_class().get_name() == 'WaterBrushManager':
                    actor.force_update()
            log('bowl depth at the centre: %.0f cm' % lib.update_landscape())
        elif step == 3 and frames > 600:
            log('bowl depth before saving: %.0f cm' % lib.update_landscape())
            log('baked %d ground cells' % lib.bake_ground())
            world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
            if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
                raise RuntimeError('could not save ' + MAP)
            unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
            log('saved; actors: ' + ', '.join(sorted({a.get_class().get_name() for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()})))
            state.update(step=4, frames=0)
        elif step == 4 and frames > 10:
            unreal.unregister_slate_post_tick_callback(state['handle'])
            log('done')
            unreal.SystemLibrary.quit_editor()
    except Exception as error:
        unreal.log_error('QUIET_LAKE_LEVEL failed at step %d: %s' % (step, error))
        unreal.unregister_slate_post_tick_callback(state['handle'])
        unreal.SystemLibrary.quit_editor()
    finally:
        state['busy'] = False


state['handle'] = unreal.register_slate_post_tick_callback(tick)
