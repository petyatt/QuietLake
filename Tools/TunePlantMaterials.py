"""Give the plants grown in the Procedural Vegetation Editor their own bark (UE Python commandlet).

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/TunePlantMaterials.py

The editor's starter trees all wear the same brown oak-like bark. This imports the birch and alder bark
atlases (Tools/GeneratedPlants/T_*_Bark_C.png from Tools/GeneratePlantTextures.py; virtual textures, as
the starter master material samples them) and makes two instances of that material in
/Game/Plants/PV/Materials: MI_Birch_Bark (white, almost smooth) and MI_Alder_Bark (dark grey, the starter
bark's furrows in relief). Then every bark slot of
SM_PV_Birch_* and SM_PV_Alder_* gets its bark. Run it again after exporting the plants anew, because
an export puts the starter materials back. Log: Saved/TunePlantMaterials.txt.
"""
import os

import unreal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAMPLES = '/ProceduralVegetationEditor/SampleAssets/'
MASTER = SAMPLES + 'Materials/MasterMaterials/MA_Foliage_Trees'
BARK = SAMPLES + 'StarterContent/DeciduousTree_01/Textures/T_LeafTree_01_Bark_'
DST = '/Game/Plants/PV/Materials'
TEXTURES = '/Game/Plants/PV/Textures'
lib = unreal.MaterialEditingLibrary
STARTER_BARK = unreal.load_asset(SAMPLES + 'StarterContent/DeciduousTree_01/Materials/MI_LeafTree_01_Bark')
out = []


def log(text):
    out.append(str(text))
    unreal.log('QUIET_LAKE_PLANT_MATERIALS ' + str(text))


def import_texture(name):
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', os.path.join(ROOT, 'Tools', 'GeneratedPlants', name + '.png'))
    task.set_editor_property('destination_path', TEXTURES)
    task.set_editor_property('destination_name', name)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('automated', True)
    task.set_editor_property('save', True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture = unreal.load_asset(TEXTURES + '/' + name)
    texture.set_editor_property('srgb', True)
    texture.set_editor_property('virtual_texture_streaming', True)
    unreal.EditorAssetLibrary.save_asset(texture.get_path_name())
    return texture


def instance(name, scalars, vectors, textures):
    path = DST + '/' + name
    mi = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if not mi:
        factory = unreal.MaterialInstanceConstantFactoryNew()
        mi = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, DST, unreal.MaterialInstanceConstant, factory)
    lib.set_material_instance_parent(mi, unreal.load_asset(MASTER))
    # The master material's static switches as the starter bark has them (lit trunk, tinted colour...).
    for key in lib.get_static_switch_parameter_names(mi.get_base_material()):
        lib.set_material_instance_static_switch_parameter_value(
            mi, key, lib.get_material_instance_static_switch_parameter_value(STARTER_BARK, key))
    for key, value in scalars.items():
        lib.set_material_instance_scalar_parameter_value(mi, key, value)
    for key, value in vectors.items():
        lib.set_material_instance_vector_parameter_value(mi, key, unreal.LinearColor(*value))
    for key, value in textures.items():
        lib.set_material_instance_texture_parameter_value(mi, key, value)
    lib.update_material_instance(mi)
    unreal.EditorAssetLibrary.save_asset(path)
    return mi


birch_texture = import_texture('T_Birch_Bark_C')
alder_texture = import_texture('T_Alder_Bark_C')
normal = unreal.load_asset(BARK + 'NAH')
# Shared with the starter bark: specular and roughness the same. The tint's alpha is how much of it
# is laid over the texture: none, the textures have their colours already.
common = {'Specular': 1.2, 'Roughness AO': 0.7, 'Roughness Contrast': 0.7, 'Roughness Strength': 3.0}
birch = instance('MI_Birch_Bark', dict(common, **{'Normal Strength': 0.35, 'AO Strength': 0.3}),
                 {'BaseColor Tint': (0.5, 0.5, 0.5, 0.0), 'BaseColor Controls': (1.0, 1.0, 1.0, 0.0)},
                 {'Base Color': birch_texture, 'Normal': normal})
alder = instance('MI_Alder_Bark', dict(common, **{'Normal Strength': 1.0}),
                 {'BaseColor Tint': (0.5, 0.5, 0.5, 0.0), 'BaseColor Controls': (1.0, 1.0, 1.0, 0.0)},
                 {'Base Color': alder_texture, 'Normal': normal})

for prefix, material in (('Birch', birch), ('Alder', alder)):
    for name in unreal.EditorAssetLibrary.list_assets('/Game/Plants/PV', recursive=False):
        mesh_name = name.split('/')[-1].split('.')[0]
        if not mesh_name.startswith('SM_PV_' + prefix):
            continue
        mesh = unreal.load_asset(name)
        changed = []
        for index, slot in enumerate(mesh.get_editor_property('static_materials')):
            current = slot.get_editor_property('material_interface')
            if current and 'Bark' in current.get_name():
                changed.append(current.get_name())
                mesh.set_material(index, material)
        unreal.EditorAssetLibrary.save_asset(mesh.get_path_name(), only_if_is_dirty=False)
        log('%s: %s -> %s' % (mesh_name, changed, material.get_name()))

with open(os.path.join(ROOT, 'Saved', 'TunePlantMaterials.txt'), 'w', encoding='utf-8') as f:
    f.write('\n'.join(out))
