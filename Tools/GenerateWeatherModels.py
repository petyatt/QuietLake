"""Generate the rain streak tile as an FBX file (run with Blender 5.x).

    blender --background --factory-startup --python Tools/GenerateWeatherModels.py

SM_RainTile: a 12 x 12 m tile, 14 m tall, of 1000 thin rain streaks (two crossed quads each).
The streaks do not move in the mesh; M_Rain animates them. Vertex colour per streak:
  R = fall phase, G = which rain intensity shows it (a streak is drawn when G < Rain),
  B = its height in the tile (0..1), A = 0 at the top of a streak, 1 at its bottom.
Nine tiles around the camera, snapped to a 12 m grid, give world-fixed rain.
Output: Tools/GeneratedWeather/SM_RainTile.fbx
"""
import os
import random
import bmesh
import bpy
from mathutils import Vector

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'GeneratedWeather')
TILE, HEIGHT, COUNT, LENGTH, WIDTH = 12.0, 14.0, 1000, 0.55, 0.004


def main():
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    rnd = random.Random(2026)
    bm = bmesh.new()
    col = bm.loops.layers.color.new('Col')
    for _ in range(COUNT):
        x, y = rnd.uniform(0, TILE), rnd.uniform(0, TILE)
        b = rnd.random()
        z = b * HEIGHT
        phase, gate = rnd.random(), rnd.random()
        for dx, dy in ((WIDTH, 0), (0, WIDTH)):
            v = [bm.verts.new(Vector((x - dx, y - dy, z))), bm.verts.new(Vector((x + dx, y + dy, z))),
                 bm.verts.new(Vector((x + dx, y + dy, z - LENGTH))), bm.verts.new(Vector((x - dx, y - dy, z - LENGTH)))]
            f = bm.faces.new(v)
            for loop, a in zip(f.loops, (0.0, 0.0, 1.0, 1.0)):
                loop[col] = (phase, gate, b, a)
    mesh = bpy.data.meshes.new('SM_RainTile')
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(bpy.data.materials.new('M_Rain'))
    obj = bpy.data.objects.new('SM_RainTile', mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    path = os.path.join(OUT, 'SM_RainTile.fbx')
    bpy.ops.export_scene.fbx(filepath=path, use_selection=True, apply_unit_scale=True, object_types={'MESH'},
                             mesh_smooth_type='FACE', colors_type='LINEAR', axis_forward='-Y', axis_up='Z',
                             bake_space_transform=True)
    print('WEATHER_EXPORTED', len(mesh.vertices), path)


main()
