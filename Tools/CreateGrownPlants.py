"""Prepare the plants of the lake in the Procedural Vegetation Editor (run as a UE Python commandlet).

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/CreateGrownPlants.py

Each plant is a copy of one of the editor's starter graphs in /Game/Plants/PV: conifers from the conifer
tree with the pine or spruce grower preset, birches and alders from the deciduous tree (the alder with
another leaf-tree preset on its trunk grower), shrubs from the deciduous shrub; variants of one species
differ by the growers' RandomSeed (the PCG nodes' own seed does not change the plant). Every graph exports a static
Nanite mesh SM_PV_<Name> with trunk collision into /Game/Plants/PV, the kind PCG Biome Core places.
The export itself is in the editor only: open the graph and press Ctrl+E (Tools/ExportGrownPlants.py
opens them all). Then Tools/TunePlantMaterials.py gives the birches and alders their bark.
QUIETLAKE_PLANTS=Pine_B,Alder_A remakes only those graphs. Log: Saved/CreateGrownPlants.txt.
"""
import os

import unreal

SAMPLES = '/ProceduralVegetationEditor/SampleAssets/StarterContent/'
PRESETS = SAMPLES + 'Presets/'
DST = '/Game/Plants/PV/'
CONIFER = SAMPLES + 'ConiferTree_01/PVE_Sample_Conifer_Tree_01'
DECIDUOUS = SAMPLES + 'DeciduousTree_01/PVE_Sample_Deciduous_Tree_01'
SHRUB = SAMPLES + 'Deciduous_Shrub_01/PVE_Sample_Deciduous_Shrub_01'
# name: (starter graph, grower preset or None, RandomSeed of every grower or None to keep the graph's).
# The preset goes to the grower that already has one, else to the trunk grower (the most growth cycles).
PLANTS = {
    'Pine_A': (CONIFER, 'PVE_Preset_Pine_01', None),
    'Pine_B': (CONIFER, 'PVE_Preset_Pine_01', 4127),
    'Spruce_A': (CONIFER, 'PVE_Preset_Spruce_01', None),
    'Spruce_B': (CONIFER, 'PVE_Preset_Spruce_02', None),
    'Birch_A': (DECIDUOUS, None, None),
    'Birch_B': (DECIDUOUS, None, 3181),
    'Alder_A': (DECIDUOUS, 'PVE_Preset_Leaf_Tree_02', 5519),
    'Shrub_A': (SHRUB, None, None),
    'Shrub_B': (SHRUB, None, 2290),
}
ONLY = [n.strip() for n in os.environ.get('QUIETLAKE_PLANTS', '').split(',') if n.strip()]

out = []
def log(text):
    out.append(str(text))
    unreal.log('QUIET_LAKE_PLANTS ' + str(text))

unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous(['/ProceduralVegetationEditor', '/Game/Plants'], True)
for name, (source, preset, seed) in PLANTS.items():
    if ONLY and name not in ONLY:
        continue
    dst = DST + 'PV_' + name
    # A graph made before is changed in place (its exported mesh refers to it, so it cannot be deleted
    # here); every value below is set outright, so the result is the same as from a fresh copy.
    if not unreal.EditorAssetLibrary.does_asset_exist(dst) and not unreal.EditorAssetLibrary.duplicate_asset(source, dst):
        raise RuntimeError('Cannot copy %s to %s' % (source, dst))
    asset = unreal.load_asset(dst)
    package = asset.get_outermost().get_name()
    nodes = 0
    growers = []
    for node in unreal.ObjectIterator(unreal.PCGNode):
        try:
            if node.get_outermost().get_name() != package:
                continue
        except Exception:
            continue
        settings = node.get_settings()
        if not settings:
            continue
        cls = settings.get_class().get_name()
        if not cls.startswith('PV'):
            continue
        nodes += 1
        if cls == 'PVGrowerSettings':
            growers.append(settings)
        if cls == 'PVExportSettings':
            params = settings.get_editor_property('export_settings')
            folder = unreal.DirectoryPath()
            folder.set_editor_property('path', DST.rstrip('/'))
            params.set_editor_property('content_browser_folder', folder)
            params.set_editor_property('mesh_name', 'SM_PV_' + name)
            params.set_editor_property('replace_policy', unreal.PVAssetReplacePolicy.REPLACE)
            params.set_editor_property('export_mesh_type', unreal.PVExportMeshType.STATIC_MESH)
            params.set_editor_property('create_nanite_foliage', True)
            params.set_editor_property('collision', True)
            params.set_editor_property('collision_generation', unreal.PVCollisionGeneration.TRUNK_ONLY)
            settings.set_editor_property('export_settings', params)
    if preset:
        # Assigning a preset copies its grower parameters into the node.
        target = [g for g in growers if g.get_editor_property('preset')]
        if not target:
            target = [max(growers, key=lambda g: g.get_editor_property('grower_params').get_editor_property('growth_cycles'))]
        for grower in target:
            grower.set_editor_property('preset', unreal.load_asset(PRESETS + preset))
    if seed is not None:
        for grower in growers:
            params = grower.get_editor_property('grower_params')
            params.set_editor_property('random_seed', seed)
            grower.set_editor_property('grower_params', params)
    seeds = [g.get_editor_property('grower_params').get_editor_property('random_seed') for g in growers]
    unreal.EditorAssetLibrary.save_asset(dst, only_if_is_dirty=False)
    log('%s from %s, preset %s, grower seeds %s, %d nodes' % (dst, source.split('/')[-1], preset, seeds, nodes))

with open(r'C:\Fish\Saved\CreateGrownPlants.txt', 'w', encoding='utf-8') as f:
    f.write('\n'.join(out))
