"""Turn a ready-made fish model (Meshy and the like) into a project fish mesh.

    QUIETLAKE_MESHY=<source.fbx> QUIETLAKE_SPECIES=Crucian [QUIETLAKE_TRIS=8000] \
        blender --background --factory-startup --python Tools/ProcessMeshyFish.py

The project expects a fish with its origin at the snout, the body along +X, +Z up, length in
metres, and UV0.x running 0 at the snout to 1 at the tail (the swim wave and the bend in the
material are driven by it). A bought model comes in its own axes with a texture UV, so this
script rotates and centres it, keeps the texture UV in a second channel, builds the wave channel
in UV0 and decimates the mesh to a game weight. Output: Tools/GeneratedFish/SM_Fish_<Species>.fbx
"""
import math
import os

import bpy
from mathutils import Matrix

SOURCE = os.environ.get('QUIETLAKE_MESHY', '').strip()
SPECIES = os.environ.get('QUIETLAKE_SPECIES', '').strip()
# The asset it becomes, e.g. SM_Fish_Meshy_Bream (the project names bought models that way);
# by default the procedural naming, SM_Fish_<Species>.
OUTNAME = os.environ.get('QUIETLAKE_OUTNAME', '').strip() or ('SM_Fish_' + SPECIES)
TRIS = int(os.environ.get('QUIETLAKE_TRIS', '8000'))
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'GeneratedFish')

if not SOURCE or not SPECIES:
    raise SystemExit('QUIETLAKE_MESHY and QUIETLAKE_SPECIES are required')

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=SOURCE)

meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
if not meshes:
    raise SystemExit('no mesh in ' + SOURCE)
for extra in [o for o in bpy.context.scene.objects if o.type != 'MESH']:
    bpy.data.objects.remove(extra, do_unlink=True)
bpy.ops.object.select_all(action='DESELECT')
for m in meshes:
    m.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]
if len(meshes) > 1:
    bpy.ops.object.join()
obj = bpy.context.view_layer.objects.active

# The source lies with the nose towards -Y (a Meshy export): turn it so the nose looks along +X
# and the body runs towards +X behind it.
obj.rotation_euler = (0.0, 0.0, math.radians(-90.0))
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

# Keep the texture UV; the swim wave needs its own coordinate along the body.
tex_uv = obj.data.uv_layers[0].name if obj.data.uv_layers else None
for layer in list(obj.data.uv_layers):
    if layer.name != tex_uv:
        obj.data.uv_layers.remove(layer)

# Decimate to a game weight: hundreds of fish swim in the pond.
triangles = sum(len(p.vertices) - 2 for p in obj.data.polygons)
if triangles > TRIS:
    mod = obj.modifiers.new('Decimate', 'DECIMATE')
    mod.ratio = TRIS / float(triangles)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.modifier_apply(modifier=mod.name)
    print('DECIMATED', triangles, '->', sum(len(p.vertices) - 2 for p in obj.data.polygons))

# Nose at the origin, body along +X, the body's centre on the actor's origin.
co = [obj.matrix_world @ v.co for v in obj.data.vertices]
lo = [min(p[i] for p in co) for i in range(3)]
hi = [max(p[i] for p in co) for i in range(3)]
length = hi[0] - lo[0]
obj.data.transform(Matrix.Translation((-lo[0], -(lo[1] + hi[1]) * 0.5, -(lo[2] + hi[2]) * 0.5)))
obj.data.update()

# The swim wave needs a coordinate along the body. It travels in the vertex colours (red channel):
# a second UV set is not carried through FBX into the engine reliably, and the texture needs UV0.
colour_layer = obj.data.color_attributes.new(name='Col', type='BYTE_COLOR', domain='CORNER')
obj.data.color_attributes.active_color_index = 0
for poly in obj.data.polygons:
    for li in poly.loop_indices:
        v = obj.data.vertices[obj.data.loops[li].vertex_index].co
        u = min(1.0, max(0.0, v.x / length))
        colour_layer.data[li].color = (u, 1.0, 1.0, 1.0)

bpy.ops.object.select_all(action='DESELECT')
obj.select_set(True)
bpy.context.view_layer.objects.active = obj
path = os.path.join(OUT, OUTNAME + '.fbx')
bpy.ops.export_scene.fbx(filepath=path, use_selection=True, apply_unit_scale=True,
                         object_types={'MESH'}, mesh_smooth_type='FACE', colors_type='LINEAR',
                         axis_forward='-Y', axis_up='Z', bake_space_transform=True)
print('MESHY_FISH %s length %.3f m, %d triangles, uv %s, %s' % (
    SPECIES, length, sum(len(p.vertices) - 2 for p in obj.data.polygons),
    [l.name for l in obj.data.uv_layers], path))
