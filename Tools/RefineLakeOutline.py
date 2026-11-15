"""Rebuild just the Water lake of /Game/Maps/Lake with a finer waterline: no level rebuild.

The lake's spline is the waterline resampled every SpacingCm (ULakeLevelLibrary::LakeOutline). At
600 cm the straight chords between points cut inside the bends of the bank, so the water surface
stops a metre or two short of the land in places (the camp bank above all) and a strip of the bed
lies dry. This script respawns the lake at a finer step, lets the water brushes render, sets the
land back to the designed ground, bakes the heights and saves the map. The Landscape, the
vegetation and everything else in the level stay as they are.

    UnrealEditor.exe C:/Fish/QuietLake.uproject -ExecCmds="py C:/Fish/Tools/RefineLakeOutline.py"

Settings (environment): QUIETLAKE_LAKE_SPACING (cm, default 150), QUIETLAKE_LAKE_DEPTH (cm,
default 600), QUIETLAKE_LAKE_RAMP (cm, default 6000). The full rebuild BuildLakeLevel.py uses the
same spacing setting.
"""
import os

import unreal

MAP = '/Game/Maps/Lake'
SPACING = float(os.environ.get('QUIETLAKE_LAKE_SPACING', '150'))
DEPTH = float(os.environ.get('QUIETLAKE_LAKE_DEPTH', '600'))
RAMP = float(os.environ.get('QUIETLAKE_LAKE_RAMP', '6000'))
lib = unreal.LakeLevelLibrary
state = {'step': 0, 'frames': 0, 'handle': None, 'busy': False, 'tried_load': False}


def log(text):
    unreal.log('QUIET_LAKE_LEVEL ' + text)


def actors():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()


def lakes():
    return [a for a in actors() if a.get_class().get_name() == 'WaterBodyLake']


def finish():
    if state['handle'] is not None:
        unreal.unregister_slate_post_tick_callback(state['handle'])
        state['handle'] = None
    unreal.SystemLibrary.quit_editor()


def tick(delta):
    # Long steps show progress dialogs that pump editor frames: never run a step inside another.
    if state.get('busy'):
        return
    state['frames'] += 1
    step, frames = state['step'], state['frames']
    state['busy'] = True
    try:
        if step == 0 and frames > 30:
            if not lakes():
                if state['tried_load']:
                    log('no water body in %s: nothing to refine' % MAP)
                    finish()
                    return
                state['tried_load'] = True
                unreal.EditorLoadingAndSavingUtils.load_map(MAP)
                state.update(frames=0)
                return
            unreal.SystemLibrary.execute_console_command(None, 'landscape.BatchedMerge.MaxResolutionPerRenderBatch 2048')
            log('refining the lake: spacing %.0f cm, depth %.0f cm, ramp %.0f cm' % (SPACING, DEPTH, RAMP))
            state.update(step=1, frames=0)
        elif step == 1 and frames > 30:
            profile = lib.create_bed_profile('/Game/Maps', 'LakeBedProfile')
            unreal.EditorAssetLibrary.save_loaded_asset(profile)
            lib.spawn_lake(DEPTH, RAMP, SPACING, profile)
            lib.spawn_streams(60)
            lib.spawn_shallows(140, 500, 400, 100, 12)
            state.update(step=2, frames=0)
        elif step == 2 and frames in (120, 300, 480):
            # Ask the water brush manager to render the brushes again, then merge and measure.
            for actor in actors():
                if actor.get_class().get_name() == 'WaterBrushManager':
                    actor.force_update()
            log('bowl depth at the centre: %.0f cm' % lib.update_landscape())
        elif step == 2 and frames > 600:
            log('bowl depth before saving: %.0f cm' % lib.update_landscape())
            log('land set back to the design: %d vertices' % lib.rebuild_land_without_brush())
            log('baked %d ground cells' % lib.bake_ground())
            world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
            if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
                raise RuntimeError('could not save ' + MAP)
            unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
            log('saved; water bodies: ' + ', '.join(sorted({a.get_class().get_name() for a in actors() if 'Water' in a.get_class().get_name()})))
            state.update(step=3, frames=0)
        elif step == 3 and frames > 10:
            finish()
    except Exception as error:
        unreal.log_error('QUIET_LAKE_LEVEL refine failed at step %d: %s' % (step, error))
        finish()
    finally:
        state['busy'] = False


state['handle'] = unreal.register_slate_post_tick_callback(tick)
