"""Prepare the bought bottom props (logs, branches, a shell, a crayfish) as game assets: centred,
lying on the bed (longest axis along +X, lowest point at Z = 0), real size, three LODs.

    blender --background --factory-startup --python Tools/PrepareBottomProps.py
"""
import json
import math
import os
from pathlib import Path

import bpy
from mathutils import Matrix, Vector

SOURCE = Path(os.environ.get('QUIETLAKE_BOTTOM_ASSETS', 'C:/Fish/Saved/BottomAssets'))
OUT = Path('C:/Fish/Tools/GeneratedBottom/Props')
OUT.mkdir(parents=True, exist_ok=True)

# source folder prefix, asset name, target longest size (m), triangle budgets
PROPS = [
    ('sunken_decayed_log', 'Log', 2.20, (700, 220, 60)),
    ('Submerged_Branch_01', 'Branch1', 0.70, (350, 110, 36)),
    ('Submerged_Branch_02', 'Branch2', 0.90, (350, 110, 36)),
    ('Submerged_Branch_03', 'Branch3', 0.90, (350, 110, 36)),
    ('anodonta_shell', 'Shell', 0.11, (250, 80, 24)),
    ('crayfish_lowpoly', 'Crayfish', 0.18, (1564, 500, 150)),
]


def tris(obj):
    return sum(len(p.vertices) - 2 for p in obj.data.polygons)


def bounds(obj):
    co = [obj.matrix_world @ v.co for v in obj.data.vertices]
    lo = Vector((min(p[i] for p in co) for i in range(3)))
    hi = Vector((max(p[i] for p in co) for i in range(3)))
    return lo, hi


report = {}
for prefix, name, target, budgets in PROPS:
    pack = next((p for p in SOURCE.iterdir() if p.is_dir() and p.name.startswith(prefix)), None)
    if not pack:
        print('BOTTOM_PROP skip', prefix, flush=True)
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

    # Lay it down: the longest axis becomes +X (a log lies along the bed, not stuck in it).
    lo, hi = bounds(obj)
    size = hi - lo
    if size.z > size.x and size.z > size.y:
        obj.data.transform(Matrix.Rotation(math.radians(-90), 4, 'Y'))
    elif size.y > size.x and size.y > size.z:
        obj.data.transform(Matrix.Rotation(math.radians(90), 4, 'Z'))
    obj.data.update()

    lo, hi = bounds(obj)
    size = hi - lo
    scale = target / max(size.x, 1e-6)
    obj.data.transform(Matrix.Translation((-(lo.x + hi.x) * 0.5, -(lo.y + hi.y) * 0.5, -lo.z)))
    obj.data.transform(Matrix.Scale(scale, 4))
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
        bpy.ops.export_scene.fbx(filepath=str(OUT / ('SM_Bottom%s_LOD%d.fbx' % (name, lod))),
                                 use_selection=True, object_types={'MESH'}, apply_unit_scale=True,
                                 mesh_smooth_type='FACE', colors_type='LINEAR',
                                 axis_forward='-Y', axis_up='Z', bake_space_transform=True)
    report['Bottom' + name] = {'size_m': [round(v, 3) for v in size], 'triangles': counts}
    print('BOTTOM_PROP_PREPARED', 'Bottom' + name, report['Bottom' + name], flush=True)

(OUT / 'manifest.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print('BOTTOM_PROPS_READY', flush=True)
