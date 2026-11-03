"""Prepare the bought water-lily models (Meshy) as game assets with three LODs.

    QUIETLAKE_LILIES=<folder with a pad/ and flower/ pack>
    blender --background --factory-startup --python Tools/PrepareLilyLODs.py

A pad is a flat leaf: scaled to 28 cm across and decimated hard (it is instanced ~165 000 times,
so the triangle budget is tiny and the texture carries the look). A flower is scaled to 14 cm and
kept heavier (a few thousand instances). Output: Tools/GeneratedPlants/Meshy/SM_Lily*_LOD*.fbx.
"""
import json
import os
from pathlib import Path

import bmesh
import bpy
from mathutils import Matrix

SOURCE = Path(os.environ.get('QUIETLAKE_LILIES', 'C:/Fish/Saved/LilyModels'))
OUT = Path('C:/Fish/Tools/GeneratedPlants/Meshy')
OUT.mkdir(parents=True, exist_ok=True)

MODELS = [
    # name, source sub-folder, target width (m), triangle budgets for LOD0/1/2
    ('LilyPad', 'Кувшинка', 0.28, (300, 100, 30)),
    ('LilyFlower', 'Цветок кувшинки', 0.14, (3900,)),
]


def tris(obj):
    return sum(len(p.vertices) - 2 for p in obj.data.polygons)


def thin(obj, budget):
    """Drop the smallest separate pieces until the mesh fits.

    A flower is hundreds of petals and stamens, each its own island, so decimation stops at one
    triangle per island; removing the smallest pieces gets the low LODs down without a hole in the
    middle of the bloom."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.verts.ensure_lookup_table()
    bm.faces.ensure_lookup_table()
    seen = set()
    island_of = {}
    islands = 0
    for start in bm.verts:
        if start.index in seen:
            continue
        stack = [start]
        seen.add(start.index)
        island_of[start.index] = islands
        while stack:
            cur = stack.pop()
            for edge in cur.link_edges:
                other = edge.other_vert(cur)
                if other.index not in seen:
                    seen.add(other.index)
                    island_of[other.index] = islands
                    stack.append(other)
        islands += 1
    island_tris = [0] * islands
    for face in bm.faces:
        island_tris[island_of[face.verts[0].index]] += len(face.verts) - 2
    total = sum(island_tris)
    drop = [False] * islands
    for index in sorted(range(islands), key=lambda i: island_tris[i]):
        if total <= budget:
            break
        drop[index] = True
        total -= island_tris[index]
    if total <= budget and any(drop):
        doomed = [face for face in bm.faces if drop[island_of[face.verts[0].index]]]
        bmesh.ops.delete(bm, geom=doomed, context='FACES')
        bm.to_mesh(obj.data)
        obj.data.update()
    bm.free()
    return tris(obj)


def decimate(obj, budget):
    obj.data.calc_loop_triangles()
    count = tris(obj)
    if count > budget:
        mod = obj.modifiers.new('GameBudget', 'DECIMATE')
        mod.ratio = budget / float(count)
        bpy.ops.object.modifier_apply(modifier=mod.name)
    return tris(obj)


report = {}
for name, folder, width, budgets in MODELS:
    pack = SOURCE / folder
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
    span = max(hi[0] - lo[0], hi[1] - lo[1])
    scale = width / max(span, 1e-6)
    # Centre on the origin and put the lowest point at Z = 0 (the waterline for a pad, the petal
    # base for a flower).
    obj.data.transform(Matrix.Translation((-(lo[0] + hi[0]) * 0.5, -(lo[1] + hi[1]) * 0.5, -lo[2])))
    obj.data.transform(Matrix.Scale(scale, 4))
    obj.data.update()
    height = (hi[2] - lo[2]) * scale

    original = obj.data.copy()
    counts = []
    for lod, budget in enumerate(budgets):
        obj.data = original.copy()
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        got = decimate(obj, budget)
        if got > budget * 1.5:
            # Foliage-like meshes collapse only down to one triangle per island; weld the small
            # separate pieces first and try again.
            bpy.ops.object.mode_set(mode='EDIT')
            bpy.ops.mesh.select_all(action='SELECT')
            bpy.ops.mesh.remove_doubles(threshold=0.002)
            bpy.ops.object.mode_set(mode='OBJECT')
            got = decimate(obj, budget)
        for face in obj.data.polygons:
            face.use_smooth = True
        counts.append(got)
        bpy.ops.export_scene.fbx(filepath=str(OUT / ('SM_%s_LOD%d.fbx' % (name, lod))),
                                 use_selection=True, object_types={'MESH'}, apply_unit_scale=True,
                                 mesh_smooth_type='FACE', colors_type='LINEAR',
                                 axis_forward='-Y', axis_up='Z', bake_space_transform=True)
    report[name] = {'width_m': width, 'height_m': height, 'triangles': counts}
    print('LILY_PREPARED', name, report[name], flush=True)

(OUT / 'manifest.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print('LILIES_READY', flush=True)
