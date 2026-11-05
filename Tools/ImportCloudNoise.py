"""Import the cloud noises (Tools/GenerateCloudNoise.py) as volume textures (UE Python commandlet).

Each PNG strip of Z slices becomes a 2D texture /Game/Weather/Noise/T_Cloud<Name> and a volume
texture VT_Cloud<Name> built from it; uncompressed and linear (they are data, not colour).
"""
import os

import unreal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = '/Game/Weather/Noise'
assets = unreal.AssetToolsHelpers.get_asset_tools()

for name, size in (('CloudShape', 128), ('CloudErosion', 32)):
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', os.path.join(ROOT, 'Tools', 'GeneratedClouds', 'T_%s.png' % name))
    task.set_editor_property('destination_path', FOLDER)
    task.set_editor_property('automated', True)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('save', False)
    assets.import_asset_tasks([task])
    tex = unreal.load_asset('%s/T_%s' % (FOLDER, name))
    if not tex:
        raise RuntimeError('Import failed: ' + name)
    tex.set_editor_property('srgb', False)
    tex.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
    tex.set_editor_property('mip_gen_settings', unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    tex.set_editor_property('never_stream', True)
    unreal.EditorAssetLibrary.save_asset('%s/T_%s' % (FOLDER, name))

    path = '%s/VT_%s' % (FOLDER, name)
    vt = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if not vt:
        vt = assets.create_asset('VT_' + name, FOLDER, unreal.VolumeTexture, unreal.VolumeTextureFactory())
    vt.set_editor_property('source2d_texture', tex)
    vt.set_editor_property('source2d_tile_size_x', size)
    vt.set_editor_property('source2d_tile_size_y', size)
    vt.set_editor_property('srgb', False)
    vt.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
    vt.set_editor_property('address_mode', unreal.TextureAddress.TA_WRAP)
    unreal.EditorAssetLibrary.save_asset(path)
    unreal.log('QUIET_LAKE_CLOUD_NOISE_IMPORTED %s %dx%dx%d' % (name, vt.get_editor_property('source2d_tile_size_x'), size, size))
