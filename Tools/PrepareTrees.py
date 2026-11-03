"""Prepare the bought trees (a weeping willow, an oak) as game assets: standing upright, the foot of
the trunk at the origin, real height. One mesh each: the game draws them with Nanite (the foliage is
thousands of separate small pieces, so decimation cannot go much below ~200k triangles anyway).

    blender --background --factory-startup --python Tools/PrepareTrees.py

Sources: Saved/TreeAssets/<Meshy pack>/ (unpacked from the bought archives); QUIETLAKE_TREE_ASSETS
overrides the folder. Output: Tools/GeneratedTrees/SM_<Name>_LOD0.fbx and manifest.json; the
textures are copied next to them as T_<Name>_Base/Normal/Rough.png.
"""
import json
import math
import os
import shutil
from pathlib import Path

import bpy
from mathutils import Matrix, Vector

SOURCE = Path(os.environ.get('QUIETLAKE_TREE_ASSETS', 'C:/Fish/Saved/TreeAssets'))
OUT = Path('C:/Fish/Tools/GeneratedTrees')
OUT.mkdir(parents=True, exist_ok=True)

# source folder prefix, asset name, height (m), triangle budget
TREES = [
    ('Meshy_AI_Weeping_Tree_Green', 'Willow', 9.0, (250000,)),
    ('Meshy_AI_Green_Oak_Tree', 'Oak', 14.0, (250000,)),
]


def tris(obj):
    return sum(len(p.vertices) - 2 for p in obj.data.polygons)


def bounds(obj):
    co = [v.co for v in obj.data.vertices]
    lo = Vector((min(p[i] for p in co) for i in range(3)))
    hi = Vector((max(p[i] for p in co) for i in range(3)))
    return lo, hi


report = {}
for prefix, name, height, budgets in TREES:
    pack = next((p for p in SOURCE.iterdir() if p.is_dir() and p.name.startswith(prefix)), None)
    if not pack:
        print('TREE skip', prefix, flush=True)
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
    print('TREE source', name, tris(obj), 'triangles', flush=True)

    # Stand it up: the tallest axis becomes +Z.
    lo, hi = bounds(obj)
    size = hi - lo
    if size.y > size.z and size.y >= size.x:
        obj.data.transform(Matrix.Rotation(math.radians(90), 4, 'X'))
    elif size.x > size.z and size.x > size.y:
        obj.data.transform(Matrix.Rotation(math.radians(-90), 4, 'Y'))
    obj.data.update()
    lo, hi = bounds(obj)
    # Upside down? The crown is wider than the trunk: compare the spread near both ends.
    span = hi.z - lo.z
    def spread(z0, z1):
        pts = [v.co for v in obj.data.vertices if z0 <= v.co.z <= z1]
        if not pts:
            return 0.0
        return max(p.x for p in pts) - min(p.x for p in pts) + max(p.y for p in pts) - min(p.y for p in pts)
    if spread(lo.z, lo.z + 0.1 * span) > spread(hi.z - 0.1 * span, hi.z) * 1.5 and spread(lo.z, lo.z + 0.1 * span) > spread(lo.z + 0.4 * span, lo.z + 0.6 * span):
        obj.data.transform(Matrix.Rotation(math.radians(180), 4, 'X'))
        obj.data.update()
        lo, hi = bounds(obj)
        span = hi.z - lo.z

    # The foot of the trunk at the origin: the middle of the lowest 3% of the tree.
    foot = [v.co for v in obj.data.vertices if v.co.z <= lo.z + 0.03 * span]
    cx = sum(p.x for p in foot) / len(foot)
    cy = sum(p.y for p in foot) / len(foot)
    obj.data.transform(Matrix.Translation((-cx, -cy, -lo.z)))
    obj.data.transform(Matrix.Scale(height / max(span, 1e-6), 4))
    obj.data.update()
    lo, hi = bounds(obj)
    size = hi - lo

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
                                 mesh_smooth_type='FACE', axis_forward='-Y', axis_up='Z', bake_space_transform=True)
    for kind, suffix in (('Base', '-texture.png'), ('Normal', '-texture_normal.png'), ('Rough', '-texture_roughness.png')):
        src = next(pack.rglob('*' + suffix), None)
        if src:
            shutil.copyfile(src, OUT / ('T_%s_%s.png' % (name, kind)))
    report[name] = {'size_m': [round(v, 2) for v in size], 'triangles': counts}
    print('TREE_PREPARED', name, report[name], flush=True)

(OUT / 'manifest.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print('TREES_READY', flush=True)
