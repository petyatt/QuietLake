"""Render quick previews of generated FBX models (run with Blender 5.x).

    blender --background --factory-startup --python Tools/PreviewModels.py -- <folder> <out folder>

Each SM_*.fbx is rendered from a three-quarter view with the Workbench engine and vertex colours.
"""
import math
import os
import sys
import bpy
from mathutils import Vector

args = sys.argv[sys.argv.index('--') + 1:]
src, out = args[0], args[1]
os.makedirs(out, exist_ok=True)
for name in sorted(f for f in os.listdir(src) if f.endswith('.fbx')):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=os.path.join(src, name))
    objs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    lo = Vector((1e9, 1e9, 1e9)); hi = -lo
    for o in objs:
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            lo = Vector(map(min, lo, w)); hi = Vector(map(max, hi, w))
    centre, size = (lo + hi) / 2, (hi - lo).length
    cam = bpy.data.objects.new('Cam', bpy.data.cameras.new('Cam'))
    bpy.context.scene.collection.objects.link(cam)
    cam.location = centre + Vector((0.9, -1.1, 0.7)).normalized() * size * 1.1
    cam.rotation_euler = (centre - cam.location).to_track_quat('-Z', 'Y').to_euler()
    scene = bpy.context.scene
    scene.camera = cam
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.color_type = 'VERTEX'
    scene.display.shading.light = 'STUDIO'
    scene.render.resolution_x, scene.render.resolution_y = 800, 600
    scene.render.filepath = os.path.join(out, name.replace('.fbx', '.png'))
    bpy.ops.render.render(write_still=True)
    print('PREVIEW', name)
