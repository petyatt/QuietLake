"""Plant the lake level (/Game/Maps/Lake) with PCG Biome Core in the full editor, save it and quit:

    UnrealEditor.exe C:/Fish/QuietLake.uproject -ExecCmds="py C:/Fish/Tools/BuildLakeBiomes.py"

1. the biome maps from LakeMap (ULakeBiomeLibrary, Source/QuietLake/LakeBiomes.cpp): T_LakeBiomes over
   all the terrain for the forests, the edge and the meadows, and the finer T_LakeShoreBiomes over the
   lake for the alder strip, the reeds, the reedmace and the sedge (Saved/T_*.png to look at);
2. in /Game/Biomes: point generators (instances of Biome Core's generator graph sampling the landscape
   at their own density), a definition per biome (its name, its colour in the maps) and its plants
   (the trees and shrubs of Tools/CreateGrownPlants.py, the shore plants of Tools/GenerateShorePlants.py);
3. the actors: the Biome Core over the whole terrain and a biome texture actor per biome; the old ones
   are removed first;
4. generation (the texture actors, then the core), a check that the plants stand in their biomes
   (QUIET_LAKE_BIOMES lines), save.
The plants are spawned into instanced meshes of the core actor and saved with the level; the game does
not run PCG.
"""
import os
import runpy

import unreal

MAP = '/Game/Maps/Lake'
DST = '/Game/Biomes'
BL = unreal.LakeBiomeLibrary
CORE = '/PCGBiomeCore/'
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
state = {'step': 0, 'frames': 0, 'handle': None, 'busy': False, 'counts': None, 'still': 0}

# Generated member names of Biome Core's Blueprint structs (PCGBiomeCore 0.2).
DEF = {'Name': 'BiomeName_24_1E8586594C50912BBA59FE938C884942', 'Color': 'BiomeColor_34_BEECC81544741E47AAA35590EF6B404D',
       'Priority': 'BiomePriority_29_308259B0449F5BA935CCC9B3DBDB97F3'}
GEN = {'Type': 'GeneratorType_22_1E8586594C50912BBA59FE938C884942', 'Priority': 'GeneratorPriority_20_55EC18A64E0BBF639841499141650268',
       'Graph': 'GeneratorGraph_24_D3C696C8455C26E2F784AEA9B82FF48A'}
ASSET = {'Type': 'AssetType_205_B24771FD46D3AB5D43F4B7ACF4D116FF', 'Weight': 'Weight_28_FF7649C5487F3C34A70C388764422F88',
         'Generator': 'Generator_144_DFC8F391471C3BAC3C091FB2BE8D549F', 'Mesh': 'Mesh_119_9B34F54E4AAD5CDF4CCC908F3FB35BE0',
         'AssetOptions': 'AssetOptions_192_6659762E45847D293BE66C9FA4E2F304', 'MeshOptions': 'MeshOptions_193_2D044C97488F6BE9D02C2DBE33FD102B',
         'FilterOptions': 'FilterOptions_195_9AFD43D942610B8E6FA376928949E9F6'}
FILTER = {'HeightMin': 'HeightMin_18_7376BE1043265317D4B1EDB8C118FCEA', 'HeightMax': 'HeightMax_17_AFF8121E41FA3F3EC5BCBFBCF75E9903'}
OPT = {'Extents': 'ExtentsMultiplier_155_3774E63E484DF5C14839EFB0D4AAB83B', 'Scale': 'Scale_120_F3D0084B4BE6BF8323CE7BA7A4F9CB9B',
       'Orient': 'OrientUpward_160_EAB2AC4F40F31A0E01895CA8DB2E640C', 'SelfPrune': 'SelfPrune_136_0186E887427D45D89310B2891730C842'}
MESH = {'Collision': 'AllowCollision_229_289C7FB34A106952AF28C4903F3F55B6', 'EndCull': 'EndCullDistance_194_276A33CB4D1A67FBF6E60E960C82B5BB',
        'Shadow': 'CastShadow_180_D1FAC9314A1E04A8302843941A5D10EE',
        'DistanceField': 'AffectDistanceFieldLighting_200_7028E68A438DEF0EEE797A8EE4993857',
        'Indirect': 'AffectDynamicIndirectLighting_204_495E9BD042FF79BD2017D6BEA8C5A52D'}

# Point generators: (points per m2, point extents cm, priority; lower wins where points of two meet).
GENERATORS = {
    'Trees': (0.02, 150, 1),
    'SparseTrees': (0.006, 150, 1),
    'Shrubs': (0.008, 100, 2),
    'Reeds': (0.9, 40, 3),
    'Sedge': (0.25, 30, 4),
    'Grass': (1.0, 30, 5),
    'Ground': (0.12, 50, 6),
    'Lawn': (10.8, 15, 5),
}
PV, SHORE = '/Game/Plants/PV/SM_PV_', '/Game/Plants/Shore/SM_'
# Heights (world Z, cm; the water is at -25): trees and shrubs stand above the water, reeds no deeper than
# 1.1 m, sedge in the edge of the water at most. Small plants stay out of the distance fields and the
# Lumen scene (lit by it, lighting nothing themselves); grass and flowers cast no shadows.
TREE = dict(extents=0.3, orient=0.7, collision=True, cull=0, hmin=-5, hmax=100000, shadow=True, gi=True)
SHRUB = dict(extents=0.5, orient=0.3, collision=False, cull=25000, hmin=-5, hmax=100000, shadow=True, gi=False)
REED = dict(extents=0.35, orient=0.6, collision=False, cull=15000, hmin=-135, hmax=150, shadow=True, gi=False)
SEDGE = dict(extents=0.6, orient=0.6, collision=False, cull=15000, hmin=-60, hmax=100000, shadow=True, gi=False)
GRASS = dict(extents=0.6, orient=0.7, collision=False, cull=8000, hmin=-15, hmax=100000, shadow=False, gi=False)
FLOWER = dict(extents=0.8, orient=0.7, collision=False, cull=6000, hmin=-5, hmax=100000, shadow=False, gi=False)
LAWN = dict(GRASS, extents=(0.55, 0.55, 0.8), self_prune=False, orient=0.95, cull=4600, hmin=5)
# Biome: (map: 'all' or 'shore', cache texel cm, blending range cm, priority, [(type, mesh, weight, generator, scale, options)])
BIOMES = {
    'PineForest': ('all', 200, 400, 3, [
        ('Pine', PV + 'Pine_A', 3, 'Trees', 1.0, TREE), ('Pine', PV + 'Pine_A', 1, 'Trees', 0.85, TREE),
        ('Pine', PV + 'Pine_B', 3, 'Trees', 1.0, TREE), ('Pine', PV + 'Pine_B', 1, 'Trees', 1.1, TREE),
        ('Spruce', PV + 'Spruce_A', 0.6, 'Trees', 1.0, TREE), ('Birch', PV + 'Birch_B', 0.4, 'Trees', 1.0, TREE),
        ('Shrub', PV + 'Shrub_A', 1, 'Shrubs', 0.8, SHRUB)]),
    'SpruceForest': ('all', 200, 400, 3, [
        ('Spruce', PV + 'Spruce_A', 2, 'Trees', 1.0, TREE), ('Spruce', PV + 'Spruce_B', 3, 'Trees', 1.0, TREE),
        ('Spruce', PV + 'Spruce_B', 1, 'Trees', 0.8, TREE), ('Birch', PV + 'Birch_B', 0.5, 'Trees', 1.0, TREE),
        ('Alder', PV + 'Alder_A', 0.3, 'Trees', 1.0, TREE), ('Shrub', PV + 'Shrub_B', 1, 'Shrubs', 0.8, SHRUB)]),
    'MixedForest': ('all', 200, 400, 3, [
        ('Pine', PV + 'Pine_A', 1.2, 'Trees', 1.0, TREE), ('Pine', PV + 'Pine_B', 1.2, 'Trees', 1.0, TREE),
        ('Spruce', PV + 'Spruce_A', 1, 'Trees', 1.0, TREE), ('Spruce', PV + 'Spruce_B', 1.2, 'Trees', 1.0, TREE),
        ('Birch', PV + 'Birch_A', 1.2, 'Trees', 1.0, TREE), ('Birch', PV + 'Birch_B', 1.2, 'Trees', 1.0, TREE),
        ('Alder', PV + 'Alder_A', 0.3, 'Trees', 1.0, TREE),
        ('Shrub', PV + 'Shrub_A', 1, 'Shrubs', 1.0, SHRUB), ('Shrub', PV + 'Shrub_B', 1, 'Shrubs', 0.9, SHRUB),
        ('Grass', SHORE + 'Grass_Low', 1, 'Ground', 1.0, GRASS)]),
    'BirchGrove': ('all', 200, 400, 2, [
        ('Birch', PV + 'Birch_A', 3, 'Trees', 1.0, TREE), ('Birch', PV + 'Birch_B', 3, 'Trees', 1.0, TREE),
        ('Birch', PV + 'Birch_B', 1.5, 'Trees', 0.8, TREE), ('Pine', PV + 'Pine_B', 0.3, 'Trees', 1.0, TREE),
        ('Shrub', PV + 'Shrub_B', 1, 'Shrubs', 0.8, SHRUB), ('Grass', SHORE + 'Grass_Low', 1, 'Ground', 1.0, GRASS),
        ('Grass', SHORE + 'Grass_A', 0.5, 'Ground', 1.0, GRASS)]),
    'ForestEdge': ('all', 200, 400, 2, [
        ('Birch', PV + 'Birch_B', 1.5, 'SparseTrees', 0.9, TREE), ('Birch', PV + 'Birch_A', 0.5, 'SparseTrees', 0.9, TREE),
        ('Pine', PV + 'Pine_B', 0.7, 'SparseTrees', 0.8, TREE), ('Spruce', PV + 'Spruce_B', 0.5, 'SparseTrees', 0.7, TREE),
        ('Shrub', PV + 'Shrub_A', 2, 'Shrubs', 1.0, SHRUB), ('Shrub', PV + 'Shrub_B', 2, 'Shrubs', 0.9, SHRUB),
        ('Grass', SHORE + 'Grass_B', 2, 'Grass', 1.0, GRASS), ('Grass', SHORE + 'Grass_A', 1, 'Grass', 1.0, GRASS),
        ('Flowers', SHORE + 'Flowers_B', 0.4, 'Grass', 1.0, FLOWER)]),
    'Meadow': ('all', 200, 300, 4, [
        ('Grass', SHORE + 'Grass_A', 3, 'Grass', 1.0, GRASS), ('Grass', SHORE + 'Grass_B', 1.5, 'Grass', 1.0, GRASS),
        ('Flowers', SHORE + 'Flowers_A', 0.6, 'Grass', 1.0, FLOWER), ('Flowers', SHORE + 'Flowers_B', 0.5, 'Grass', 1.0, FLOWER)]),
    'VillageLawn': ('all', 70, 150, 4, [
        ('Grass', SHORE + 'Lawn_A', 3.0, 'Lawn', 1.0, LAWN),
        ('Grass', SHORE + 'Lawn_B', 2.0, 'Lawn', 0.93, LAWN),
        ('Flowers', SHORE + 'Flowers_A', 0.04, 'Lawn', 0.40, LAWN)]),
    'AlderBank': ('shore', 100, 50, 1, [
        ('Alder', PV + 'Alder_A', 3, 'Trees', 1.0, TREE), ('Alder', PV + 'Alder_A', 1.5, 'Trees', 0.8, TREE),
        ('Birch', PV + 'Birch_B', 0.7, 'Trees', 0.9, TREE), ('Spruce', PV + 'Spruce_B', 0.3, 'Trees', 0.8, TREE),
        ('Shrub', PV + 'Shrub_A', 1, 'Shrubs', 0.8, SHRUB), ('Shrub', PV + 'Shrub_B', 1, 'Shrubs', 0.8, SHRUB),
        ('Sedge', SHORE + 'Sedge_A', 1, 'Sedge', 1.0, SEDGE), ('Sedge', SHORE + 'Sedge_B', 1, 'Sedge', 1.0, SEDGE),
        ('Grass', SHORE + 'Grass_Low', 1, 'Ground', 1.0, GRASS)]),
    'ReedBelt': ('shore', 100, 50, 1, [
        ('Reed', SHORE + 'Reed_A', 2, 'Reeds', 1.0, REED), ('Reed', SHORE + 'Reed_B', 2, 'Reeds', 1.0, REED),
        ('Bulrush', SHORE + 'Bulrush', 1, 'Reeds', 1.0, REED), ('Sedge', SHORE + 'Sedge_A', 0.6, 'Reeds', 1.0, SEDGE)]),
    'Cattails': ('shore', 100, 50, 1, [
        ('Bulrush', SHORE + 'Bulrush', 4, 'Reeds', 1.0, REED), ('Reed', SHORE + 'Reed_A', 0.5, 'Reeds', 1.0, REED)]),
    'SedgeTussocks': ('shore', 100, 50, 1, [
        ('Sedge', SHORE + 'Sedge_A', 2, 'Sedge', 1.0, SEDGE), ('Sedge', SHORE + 'Sedge_B', 2, 'Sedge', 1.0, SEDGE)]),
}
# A row of identical trees reads as a stamping press: the scale of a PCG asset is one fixed number, so
# the height range is made of several weighted copies of the same mesh (the table's own scale variants are
# merged into one weight first).
TREE_SCALES = ((0.58, 0.10), (0.76, 0.22), (0.95, 0.36), (1.16, 0.22), (1.38, 0.10))
SHRUB_SCALES = ((0.70, 0.20), (0.85, 0.35), (1.00, 0.30), (1.20, 0.15))


def vary_heights(plants):
    merged = []
    index = {}
    for kind, mesh, weight, gen, scale, o in plants:
        if kind in ('Pine', 'Spruce', 'Birch', 'Alder', 'Shrub'):
            key = (kind, mesh, gen, tuple(sorted(o.items())))
            if key in index:
                old = merged[index[key]]
                merged[index[key]] = (old[0], old[1], old[2] + weight, old[3], old[4], old[5])
            else:
                index[key] = len(merged)
                merged.append((kind, mesh, weight, gen, 1.0, o))
        else:
            merged.append((kind, mesh, weight, gen, scale, o))
    out = []
    for kind, mesh, weight, gen, scale, o in merged:
        if kind in ('Pine', 'Spruce', 'Birch', 'Alder', 'Shrub'):
            for f, share in (SHRUB_SCALES if kind == 'Shrub' else TREE_SCALES):
                out.append((kind, mesh, weight * share, gen, scale * f, o))
        else:
            out.append((kind, mesh, weight, gen, scale, o))
    return out


BIOMES = {name: (m, texel, blend, prio, vary_heights(plants)) for name, (m, texel, blend, prio, plants) in BIOMES.items()}

# QUIETLAKE_BIOMES_ONLY=ReedBelt,PineForest plants only those (for trying settings out).
ONLY = [n for n in os.environ.get('QUIETLAKE_BIOMES_ONLY', '').split(',') if n]
if ONLY:
    BIOMES = {k: v for k, v in BIOMES.items() if k in ONLY}


def log(text):
    unreal.log('QUIET_LAKE_BIOMES ' + str(text))


def ref(cls, path):
    name = path.split('/')[-1]
    return '"%s\'%s.%s\'"' % (cls, path, name)


def set_text(obj, prop, text):
    if not BL.set_property_text(obj, prop, text):
        raise RuntimeError('cannot set %s on %s to %s' % (prop, obj.get_name(), text[:200]))


def fresh(src, dst):
    """A copy of a Biome Core default asset to fill in (reused if it exists)."""
    if unreal.EditorAssetLibrary.does_asset_exist(dst):
        return unreal.load_asset(dst)
    if not unreal.EditorAssetLibrary.duplicate_asset(src, dst):
        raise RuntimeError('cannot copy %s to %s' % (src, dst))
    return unreal.load_asset(dst)


def make_generator(name, density, extents, priority, seed):
    path = '%s/Generators/GI_%s' % (DST, name)
    instance = BL.make_graph_instance(path, unreal.load_asset(CORE + 'GraphTemplates/TPL_BiomeCore_Generator'))
    P = unreal.PCGGraphParametersHelpers
    P.set_bool_parameter(instance, 'SampleOnLandscape', True)
    P.set_double_parameter(instance, 'PointsPerSquaredMeter', density)
    P.set_vector_parameter(instance, 'PointExtents', unreal.Vector(extents, extents, extents))
    P.set_double_parameter(instance, 'PointsLooseness', 1.0)
    P.set_int32_parameter(instance, 'Seed', seed)
    got = (P.get_bool_parameter(instance, 'SampleOnLandscape'), P.get_double_parameter(instance, 'PointsPerSquaredMeter'),
           P.get_vector_parameter(instance, 'PointExtents').x, P.get_int32_parameter(instance, 'Seed'))
    if not got[0] or abs(got[1] - density) > 1e-6 or abs(got[2] - extents) > 1e-3:
        raise RuntimeError('generator %s parameters not set: %s' % (name, got))
    log('generator %s: landscape %s, %.3f points/m2, extents %.0f, seed %d' % ((name,) + got))
    unreal.EditorAssetLibrary.save_loaded_asset(instance)
    generator = fresh(CORE + 'BiomeGenerators/DefaultGenerator', '%s/Generators/BG_%s' % (DST, name))
    set_text(generator, 'Generator', '(%s="%s",%s=%d,%s=%s)' % (GEN['Type'], name, GEN['Priority'], priority, GEN['Graph'],
                                                                ref('/Script/PCG.PCGGraphInstance', path)))
    unreal.EditorAssetLibrary.save_loaded_asset(generator)
    return generator.get_path_name().split('.')[0]


def entry(kind, mesh, weight, generator, scale, o):
    s = scale
    extents = o['extents']
    extent_text = '(X=%f,Y=%f,Z=%f)' % extents if isinstance(extents, tuple) else '%f' % extents
    return '(' + ','.join([
        '%s="%s"' % (ASSET['Type'], kind),
        '%s=%f' % (ASSET['Weight'], weight),
        '%s=%s' % (ASSET['Generator'], ref('/PCGBiomeCore/BiomeGenerators/Setup/BiomeGeneratorTemplate.BiomeGeneratorTemplate_C', generator)),
        '%s=%s' % (ASSET['Mesh'], ref('/Script/Engine.StaticMesh', mesh)),
        '%s=(%s=(X=%f,Y=%f,Z=%f),%s=%s,%s=%f,%s=%s)' % (ASSET['AssetOptions'], OPT['Scale'], s, s, s, OPT['Extents'], extent_text,
                                                        OPT['Orient'], o['orient'], OPT['SelfPrune'], o.get('self_prune', True)),
        '%s=(%s=%s,%s=%d,%s=%s,%s=%s,%s=%s)' % (ASSET['MeshOptions'], MESH['Collision'], o['collision'], MESH['EndCull'], o['cull'],
                                               MESH['Shadow'], o['shadow'], MESH['DistanceField'], o['gi'], MESH['Indirect'], o['gi']),
        '%s=(%s=%f,%s=%f)' % (ASSET['FilterOptions'], FILTER['HeightMin'], o['hmin'], FILTER['HeightMax'], o['hmax']),
    ]) + ')'


def make_assets():
    generators = {}
    for seed, (name, (density, extents, priority)) in enumerate(GENERATORS.items()):
        generators[name] = make_generator(name, density, extents, priority, 1301 + seed * 977)
    made = {}
    for name, (kind, texel, blend, priority, plants) in BIOMES.items():
        definition = fresh(CORE + 'BiomeDefinitions/DefaultBiome', '%s/Definitions/BD_%s' % (DST, name))
        c = BL.biome_color(name)
        set_text(definition, 'BiomeDefinition', '(%s="%s",%s=(R=%f,G=%f,B=%f,A=0.000000),%s=%d)' % (DEF['Name'], name, DEF['Color'], c.r, c.g, c.b, DEF['Priority'], priority))
        unreal.EditorAssetLibrary.save_loaded_asset(definition)
        asset = fresh(CORE + 'BiomeAssets/DefaultAsset', '%s/Assets/BA_%s' % (DST, name))
        set_text(asset, 'BiomeAssets', '(' + ','.join(entry(k, m, w, generators[g], s, o) for k, m, w, g, s, o in plants) + ')')
        unreal.EditorAssetLibrary.save_loaded_asset(asset)
        made[name] = (definition, asset)
        log('biome %s: %d plants' % (name, len(plants)))
    return made


def on_demand(actor):
    """PCG of the actor runs only when asked (not on every property change while it is set up)."""
    for c in actor.get_components_by_class(unreal.PCGComponent):
        c.cleanup_local(True)
        c.set_editor_property('regenerate_in_editor', False)
        c.set_editor_property('generation_trigger', unreal.PCGComponentGenerationTrigger.GENERATE_ON_DEMAND)


def box_actor(cls_path, name, rect, box_prop, z_half=4000, z_mid=2000, component_scale=False):
    """An actor of a Biome Core class whose box covers rect (min x, min y, max x, max y). The actor
    itself is scaled: a change to a Blueprint property reruns the construction script, which rebuilds
    the box component with its own scale, but keeps the actor's transform. component_scale: the box
    component has a scale of its own in the Blueprint (the core's volume) to take into account."""
    cls = unreal.load_object(None, cls_path)
    centre = unreal.Vector((rect.x + rect.z) * 0.5, (rect.y + rect.w) * 0.5, z_mid)
    actor = actors.spawn_actor_from_class(cls, centre, unreal.Rotator(0, 0, 0))
    on_demand(actor)
    actor.set_actor_label(name)
    box = actor.get_editor_property(box_prop)
    e = box.get_scaled_box_extent() if component_scale else box.get_unscaled_box_extent()
    actor.set_actor_scale3d(unreal.Vector((rect.z - rect.x) * 0.5 / e.x, (rect.w - rect.y) * 0.5 / e.y, z_half / e.z))
    return actor


def box_size(actor, box_prop):
    e = actor.get_editor_property(box_prop).get_scaled_box_extent()
    return '%.0f x %.0f x %.0f m' % (e.x / 50, e.y / 50, e.z / 50)


def biome_actors():
    return [a for a in actors.get_all_level_actors() if a.get_class().get_name().startswith(('BP_PCGBiome', 'BP_PCGBiomeCore'))]


def instance_counts(core):
    counts = {}
    for c in core.get_components_by_class(unreal.InstancedStaticMeshComponent):
        mesh = c.get_editor_property('static_mesh')
        name = mesh.get_name() if mesh else '?'
        counts[name] = counts.get(name, 0) + c.get_instance_count()
    return counts


def clear_village_planting(core):
    """Keep generated plants out of the precise street and garden layout before baking."""
    tall_prefixes = ('SM_PV_', 'SM_Reed', 'SM_Bulrush', 'SM_Sedge')
    rect = BL.village_lawn_map_rect()
    removed = 0
    for component in core.get_components_by_class(unreal.InstancedStaticMeshComponent):
        mesh = component.get_editor_property('static_mesh')
        if not mesh:
            continue
        tall = mesh.get_name().startswith(tall_prefixes)
        blocked = []
        for i in range(component.get_instance_count()):
            p = component.get_instance_transform(i, True).translation
            if not (rect.x - 1000 <= p.x <= rect.z + 2200 and rect.y <= p.y <= rect.w + 2200):
                continue
            if BL.village_plant_blocked(p.x, p.y, tall) or (not tall and BL.clearing_at(p.x, p.y)):
                blocked.append(i)
        if blocked and not component.remove_instances(blocked):
            raise RuntimeError('could not clear grass from the village paths')
        removed += len(blocked)
    log('trimmed %d generated plants from the village clearings' % removed)


def check(core):
    """How many of the planted instances stand where the map puts their biome."""
    allowed = {}
    for name, (_, _, _, _, plants) in BIOMES.items():
        for _, mesh, _, _, _, _ in plants:
            allowed.setdefault(mesh.split('/')[-1], set()).add(name)
    good = bad = 0
    misplaced = {}
    for c in core.get_components_by_class(unreal.InstancedStaticMeshComponent):
        mesh = c.get_editor_property('static_mesh')
        if not mesh:
            continue
        n = c.get_instance_count()
        for i in range(0, n, max(1, n // 150)):
            t = c.get_instance_transform(i, True)
            biome = BL.biome_at(t.translation.x, t.translation.y)
            if biome in allowed.get(mesh.get_name(), ()):
                good += 1
            else:
                bad += 1
                where = biome or BL.clearing_at(t.translation.x, t.translation.y) or 'nothing'
                key = '%s in %s' % (mesh.get_name().replace('SM_PV_', '').replace('SM_', ''), where)
                misplaced[key] = misplaced.get(key, 0) + 1
    log('check: %d instances in their biome, %d elsewhere' % (good, bad))
    for key, n in sorted(misplaced.items(), key=lambda kv: -kv[1])[:25]:
        log('  elsewhere: %s %d' % (key, n))


def tick(delta):
    if state.get('busy'):
        return
    state['frames'] += 1
    step, frames = state['step'], state['frames']
    state['busy'] = True
    try:
        if step == 0 and frames > 30:
            unreal.EditorLoadingAndSavingUtils.load_map(MAP)
            state.update(step=1, frames=0)
        elif step == 1 and frames > 30:
            for shore in (False, True):
                rect = BL.biome_map_rect(shore)
                width = 2048
                height = width if not shore else int(round(width * (rect.w - rect.y) / (rect.z - rect.x)))
                path = DST + ('/T_LakeShoreBiomes' if shore else '/T_LakeBiomes')
                for line in BL.build_biome_map(path, rect, width, height).split('\n'):
                    log(line)
                unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
            if 'VillageLawn' in BIOMES:
                path = DST + '/T_VillageLawnCoverage'
                log(BL.build_biome_map(path, BL.village_lawn_map_rect(), 1024, 640, True))
                if not unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
                    raise RuntimeError('could not save the village lawn coverage')
                if os.environ.get('QUIETLAKE_TERRAIN_LAWN_ONLY') == '1':
                    runpy.run_path(os.path.join(unreal.Paths.project_dir(), 'Tools', 'CreateTerrainMaterial.py'))
            state['made'] = make_assets()
            state.update(step=2, frames=0)
        elif step == 2 and frames > 10:
            old = biome_actors()
            for a in old:
                actors.destroy_actor(a)
            log('removed %d old biome actors' % len(old))
            terrain, shore = BL.biome_map_rect(False), BL.biome_map_rect(True)
            core = box_actor(CORE + 'Blueprints/BP_PCGBiomeCore.BP_PCGBiomeCore_C', 'LakeBiomeCore', terrain, 'Volume', 6000, 2000, True)
            textures = []
            for name, (kind, texel, blend, priority, plants) in BIOMES.items():
                rect = terrain if kind == 'all' else shore
                actor = box_actor(CORE + 'Blueprints/BP_PCGBiomeTexture.BP_PCGBiomeTexture_C', 'Biome_' + name, rect, 'BiomeTextureVolume')
                definition, asset = state['made'][name]
                texture = DST + ('/T_LakeBiomes' if kind == 'all' else '/T_LakeShoreBiomes')
                set_text(actor, 'BiomeTexture', texture + '.' + texture.split('/')[-1])
                actor.set_editor_property('Definition', definition)
                actor.set_editor_property('Assets', [asset])
                actor.set_editor_property('LocalCacheTexelSize', texel)
                actor.set_editor_property('LocalCacheSnapToSurface', True)
                actor.set_editor_property('BlendingRange', float(blend))
                on_demand(actor)
                log('texture actor %s over %s, cache texel %d cm' % (name, box_size(actor, 'BiomeTextureVolume'), texel))
                textures.append(actor)
            on_demand(core)
            log('core over %s' % box_size(core, 'Volume'))
            state.update(core=core, textures=textures, step=3, frames=0)
        elif step == 3 and frames > 30:
            for actor in state['textures']:
                actor.get_editor_property('PCG_LocalBiomeCore').generate_local(True)
            log('texture actors generating')
            state.update(step=4, frames=0)
        elif step == 4 and frames > 300:
            state['core'].get_editor_property('BiomeCore').generate_local(True)
            log('core generating')
            state.update(step=5, frames=0, counts=None, still=0)
        elif step == 5 and frames % 60 == 0:
            counts = instance_counts(state['core'])
            total = sum(counts.values())
            state['still'] = state['still'] + 1 if counts == state['counts'] and total > 0 else 0
            state['counts'] = counts
            if frames % 600 == 0:
                log('generating: %d instances after %d frames' % (total, frames))
            if state['still'] >= 5 or frames > 36000:
                if 'VillageLawn' in BIOMES:
                    clear_village_planting(state['core'])
                    counts = instance_counts(state['core'])
                    total = sum(counts.values())
                for mesh, n in sorted(counts.items()):
                    log('  %s %d' % (mesh, n))
                log('total %d instances' % total)
                check(state['core'])
                # The plants stay as plain instanced meshes; the biome actors (and their PCG caches,
                # some 170 MB) are not kept in the level: this script makes them again from nothing.
                for a in actors.get_all_level_actors():
                    if a.get_actor_label() == 'LakeVegetation':
                        actors.destroy_actor(a)
                baked = BL.bake_instances(state['core'], 'LakeVegetation')
                n = sum(c.get_instance_count() for c in baked.get_components_by_class(unreal.InstancedStaticMeshComponent))
                log('baked %d instances into LakeVegetation (%d meshes)' % (n, len(baked.get_components_by_class(unreal.InstancedStaticMeshComponent))))
                if n != total:
                    raise RuntimeError('baked %d of %d instances' % (n, total))
                for a in biome_actors():
                    actors.destroy_actor(a)
                world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
                if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP):
                    raise RuntimeError('could not save ' + MAP)
                unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
                log('saved')
                state.update(step=6, frames=0)
        elif step == 6 and frames > 10:
            unreal.unregister_slate_post_tick_callback(state['handle'])
            log('done')
            if not os.environ.get('QUIETLAKE_BIOMES_STAY'):
                unreal.SystemLibrary.quit_editor()
    except Exception as error:
        unreal.log_error('QUIET_LAKE_BIOMES failed at step %d: %s' % (step, error))
        unreal.unregister_slate_post_tick_callback(state['handle'])
        if not os.environ.get('QUIETLAKE_BIOMES_STAY'):
            unreal.SystemLibrary.quit_editor()
    finally:
        state['busy'] = False


state['handle'] = unreal.register_slate_post_tick_callback(tick)
