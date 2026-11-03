import unreal
SRC = r'C:\Fish\Tools\Sounds'
FILES = [('A_Wind.ogg', 'A_Wind'), ('A_RainLight.ogg', 'A_RainLight'),
         ('A_RainHeavy.wav', 'A_RainHeavy'), ('A_Crickets.ogg', 'A_Crickets')]
for filename, name in FILES:
    path = SRC + '\\' + filename
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', path)
    task.set_editor_property('destination_path', '/Game/Audio')
    task.set_editor_property('destination_name', name)
    task.set_editor_property('automated', True)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('save', True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    wave = unreal.load_asset('/Game/Audio/' + name)
    if not wave:
        unreal.log('NATURAL_SOUND FAILED ' + name)
        continue
    looped = False
    for prop in ('looping', 'b_looping'):
        try:
            wave.set_editor_property(prop, True)
            looped = True
            break
        except Exception:
            pass
    unreal.EditorAssetLibrary.save_asset('/Game/Audio/' + name)
    unreal.log('NATURAL_SOUND %s %.1f s looped=%s' % (name, wave.get_editor_property('duration'), looped))
unreal.log('QUIET_LAKE_SOUNDS_READY')
