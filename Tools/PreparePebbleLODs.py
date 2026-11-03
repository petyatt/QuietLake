"""Prepare the bought pebbles (Meshy) as game assets with three LODs.

    blender --background --factory-startup --python Tools/PreparePebbleLODs.py

Each pebble is centred on the origin with its lowest point at Z = 0, keeps its real size, and is
decimated hard: pebbles are instanced by the hundred in the shallows, so the normal map carries the
look. Output: Tools/GeneratedBottom/Meshes/SM_Pebble_<Name>_LOD<k>.fbx.
"""
import json
import os
from pathlib import Path

import bpy
from mathutils import Matrix

SOURCE = Path(os.environ.get('QUIETLAKE_PEBBLES', 'C:/Fish/Saved/Pebbles'))
OUT = Path('C:/Fish/Tools/GeneratedBottom/Meshes')
OUT.mkdir(parents=True, exist_ok=True)

# folder prefix, asset name, triangle budgets for LOD0/1/2
PACKS = [
    ('pebble_asymmetric', 'Pebble_Asymmetric', (320, 120, 40)),
    ('pebble_flat_oval', 'Pebble_FlatOval', (600, 220, 70)),
    ('pebble_round_chunky', 'Pebble_Round', (320, 120, 40)),
    ('pebble_teardrop', 'Pebble_Teardrop', (320, 120, 40)),
]


def tris(obj):
    return sum(len(p.vertices) - 2 for p in obj.data.polygons)


report = {}
for folder, name, budgets in PACKS:
    pack = next((p for p in SOURCE.iterdir() if p.is_dir() and p.name.startswith(folder)), SOURCE / folder)
    if not pack.exists():
        print('PEBBLE skip', folder, flush=True)
        continue
    fbx = next(pack.rglob('*.fbx'))
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(fbx))
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    if not meshes:
        raise SystemExit('no mesh in ' + str(fbx))
    for extra in [o for o in bpy.context.scene.objects if o.type != 'MESH']:
        bpy.data.objects.remove(extra, do_unlink=True)
    bpy.ops.object.select_all(action='DESELECT')
    for m in meshes:
        m.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    co = [obj.matrix_world @ v.co for v in obj.data.vertices]
    lo = [min(p[i] for p in co) for i in range(3)]
    hi = [max(p[i] for p in co) for i in range(3)]
    # Centre it on the origin and sit it on the bed (Z = 0 is the bottom of the stone).
    obj.data.transform(Matrix.Translation((-(lo[0] + hi[0]) * 0.5, -(lo[1] + hi[1]) * 0.5, -lo[2])))
    obj.data.update()
    size = (hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2])

    original = obj.data.copy()
    counts = []
    for lod, budget in enumerate(budgets):
        obj.data = original.copy()
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        got = tris(obj)
        if got > budget:
            mod = obj.modifiers.new('GameBudget', 'DECIMATE')
            mod.ratio = budget / float(got)
            bpy.ops.object.modifier_apply(modifier=mod.name)
            got = tris(obj)
        for face in obj.data.polygons:
            face.use_smooth = True
        counts.append(got)
        bpy.ops.export_scene.fbx(filepath=str(OUT / ('SM_%s_LOD%d.fbx' % (name, lod))),
                                 use_selection=True, object_types={'MESH'}, apply_unit_scale=True,
                                 mesh_smooth_type='FACE', colors_type='LINEAR',
                                 axis_forward='-Y', axis_up='Z', bake_space_transform=True)
    report[name] = {'size_m': [round(s, 3) for s in size], 'triangles': counts}
    print('PEBBLE_PREPARED', name, report[name], flush=True)

(OUT / 'manifest.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print('PEBBLES_READY', flush=True)
