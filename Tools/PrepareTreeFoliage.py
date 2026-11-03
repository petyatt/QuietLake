"""Give the bought trees (willow, oak) real foliage: keep the trunk and the branches of the Meshy model,
drop its solid "leaf blobs", and grow alpha-masked leaf cards where the blobs were - twigs of lobed
leaves on the oak, long hanging strands of narrow leaves on the willow. Also draws the leaf textures.

    blender --background --factory-startup --python Tools/PrepareTreeFoliage.py

Input: Tools/GeneratedTrees/SM_<Name>_LOD0.fbx and T_<Name>_Base.png (Tools/PrepareTrees.py).
Output: Tools/GeneratedTrees/SM_<Name>Leafy.fbx (material slots: Bark, Leaves), T_<Name>Leaf.png (RGBA),
Saved/TreeRender_<Name>.png (a quick check render). QUIETLAKE_TREE_RENDER=0 skips the render.
"""
import math
import os
import random
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Vector

DIR = Path('C:/Fish/Tools/GeneratedTrees')
RENDER = os.environ.get('QUIETLAKE_TREE_RENDER', '1') != '0'


# --- leaf textures -------------------------------------------------------------------------------
def save_rgba(name, rgba):
    h, w, _ = rgba.shape
    img = bpy.data.images.new(name, w, h, alpha=True)
    img.pixels.foreach_set(np.flipud(rgba).astype(np.float32).ravel())
    img.filepath_raw = str(DIR / (name + '.png'))
    img.file_format = 'PNG'
    img.save()
    return img


def paint_leaf(canvas, cx, cy, length, width, angle, colour, lobes=0, rnd=None):
    """An elliptic (or lobed) leaf with a darker midrib, painted over canvas (H, W, 4)."""
    h, w, _ = canvas.shape
    r = int(length * 0.6 + 2)
    x0, x1, y0, y1 = max(0, int(cx - r)), min(w, int(cx + r)), max(0, int(cy - r)), min(h, int(cy + r))
    if x0 >= x1 or y0 >= y1:
        return
    ys, xs = np.mgrid[y0:y1, x0:x1]
    dx, dy = xs - cx, ys - cy
    ca, sa = math.cos(angle), math.sin(angle)
    u = dx * ca + dy * sa          # along the leaf, from its stalk
    v = -dx * sa + dy * ca
    t = u / length                 # 0 at the stalk, 1 at the tip
    half = width * 0.5 * np.sin(np.clip(t, 0, 1) * math.pi) ** 0.8
    if lobes:
        half = half * (1 + 0.28 * np.cos(t * lobes * 2 * math.pi))
    inside = (t > 0) & (t < 1) & (np.abs(v) < half)
    shade = 0.82 + 0.25 * (v / np.maximum(half, 1e-3)) * 0.5 + 0.1 * (1 - t)
    rib = np.abs(v) < max(0.8, width * 0.04)
    col = np.stack([colour[0] * shade, colour[1] * shade, colour[2] * shade], -1)
    col[rib] *= 0.75
    region = canvas[y0:y1, x0:x1]
    region[inside, :3] = col[inside]
    region[inside, 3] = 1.0


def paint_twig(canvas, pts, width, colour):
    h, w, _ = canvas.shape
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        n = int(max(abs(bx - ax), abs(by - ay))) + 1
        for k in range(n):
            x, y = ax + (bx - ax) * k / n, ay + (by - ay) * k / n
            x0, x1, y0, y1 = int(x - width), int(x + width) + 1, int(y - width), int(y + width) + 1
            canvas[max(0, y0):min(h, y1), max(0, x0):min(w, x1), :3] = colour
            canvas[max(0, y0):min(h, y1), max(0, x0):min(w, x1), 3] = 1.0


def oak_texture():
    """A leafy oak twig: a dozen lobed leaves fanning from a short twig, 1024 x 1024."""
    rnd = random.Random(3)
    c = np.zeros((1024, 1024, 4), np.float32)
    c[..., :3] = (0.12, 0.2, 0.06)          # leaf colour bleeds into the transparent edge (mip-safe)
    twig = [(512, 1010), (505, 760), (520, 520), (510, 300)]
    paint_twig(c, twig, 5, (0.22, 0.16, 0.1))
    for k in range(14):
        a = k / 13.0
        x, y = 512 + rnd.uniform(-12, 12), 960 - a * 700
        side = 1 if k % 2 else -1
        ang = -math.pi / 2 + side * rnd.uniform(0.5, 1.3)
        green = (rnd.uniform(0.16, 0.24), rnd.uniform(0.32, 0.42), rnd.uniform(0.08, 0.13))
        paint_leaf(c, x, y, rnd.uniform(260, 340), rnd.uniform(120, 160), ang, green, lobes=4)
    paint_leaf(c, 510, 330, 300, 140, -math.pi / 2, (0.2, 0.38, 0.1), lobes=4)
    return c


def willow_texture():
    """A hanging willow strand: a thin twig with narrow pointed leaves along it, 256 x 1024."""
    rnd = random.Random(5)
    c = np.zeros((1024, 256, 4), np.float32)
    c[..., :3] = (0.2, 0.28, 0.13)
    twig = [(128 + 6 * math.sin(k * 0.6), 4 + k * 102) for k in range(11)]
    paint_twig(c, twig, 2, (0.3, 0.26, 0.12))
    for k in range(46):
        y = 20 + k * 21 + rnd.uniform(-5, 5)
        x = 128 + 6 * math.sin(y / 102 * 0.6)
        side = 1 if k % 2 else -1
        ang = math.pi / 2 + side * rnd.uniform(0.25, 0.6)          # pointing down and out
        green = (rnd.uniform(0.2, 0.27), rnd.uniform(0.3, 0.37), rnd.uniform(0.13, 0.18))     # willow's grey-green
        paint_leaf(c, x, y, rnd.uniform(95, 130), rnd.uniform(14, 20), ang, green)
    return c


# --- trees ----------------------------------------------------------------------------------------
def load(name):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    # Read with the axes it was written with (Tools/PrepareTrees.py): Z up, -Y forward.
    bpy.ops.import_scene.fbx(filepath=str(DIR / ('SM_%s_LOD0.fbx' % name)), axis_forward='-Y', axis_up='Z')
    obj = [o for o in bpy.context.scene.objects if o.type == 'MESH'][0]
    for extra in [o for o in bpy.context.scene.objects if o.type != 'MESH']:
        bpy.data.objects.remove(extra, do_unlink=True)
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    # The FBX comes in as centimetres with a 0.01 scale: work in metres.
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return obj


def classify_leaf_faces(obj, texture):
    """True for faces whose texture is foliage green, False for bark."""
    img = bpy.data.images.load(str(texture))
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    px = px.reshape(h, w, 4)
    me = obj.data
    uv = me.uv_layers.active.data
    leaf = np.zeros(len(me.polygons), bool)
    for i, poly in enumerate(me.polygons):
        us = [uv[l].uv for l in poly.loop_indices]
        u = sum(p.x for p in us) / len(us) % 1.0
        v = sum(p.y for p in us) / len(us) % 1.0
        r, g, b, _ = px[min(h - 1, int(v * h)), min(w - 1, int(u * w))]
        leaf[i] = g > r * 1.05 and g > b * 1.15 and g > 0.06
    return leaf


def main():
    DIR.mkdir(parents=True, exist_ok=True)
    save_rgba('T_OakLeaf', oak_texture())
    save_rgba('T_WillowLeaf', willow_texture())
    print('FOLIAGE textures ready', flush=True)
    for name, kind in (('Willow', 'willow'), ('Oak', 'oak')):
        obj = load(name)
        me = obj.data
        leaf = classify_leaf_faces(obj, DIR / ('T_%s_Base.png' % name))
        print('FOLIAGE', name, 'faces', len(leaf), 'leaf', int(leaf.sum()), flush=True)
        # Leaf positions and normals, area-weighted, before the blobs go.
        centres = np.array([p.center[:] for p in me.polygons])
        normals = np.array([p.normal[:] for p in me.polygons])
        areas = np.array([p.area for p in me.polygons])
        # Of the bark keep only the trunk (and on the oak the bases of the main limbs): the rest of
        # Meshy's branches are crude slabs; the leaves cover where they were.
        radial = np.hypot(centres[:, 0], centres[:, 1])
        crude = (~leaf) & (centres[:, 2] > 2.0) if kind == 'willow' else (~leaf) & (radial > 0.8) & (centres[:, 2] > 1.8)
        print('FOLIAGE', name, 'crude branch faces dropped', int(crude.sum()), flush=True)
        idx = np.nonzero(leaf)[0]
        drop = np.nonzero(leaf | crude)[0]
        crown = centres[idx].mean(axis=0)
        rng = np.random.default_rng(7)
        count = 9000 if kind == 'oak' else 5200
        picks = rng.choice(idx, size=count, p=areas[idx] / areas[idx].sum())

        bm = bmesh.new()
        bm.from_mesh(me)
        bm.faces.ensure_lookup_table()
        bmesh.ops.delete(bm, geom=[bm.faces[i] for i in drop], context='FACES')
        me.materials.append(bpy.data.materials.new('Leaves'))
        slot = len(me.materials) - 1
        uvl = bm.loops.layers.uv.active

        def quad(a, b, c, d):
            vs = [bm.verts.new(p) for p in (a, b, c, d)]
            f = bm.faces.new(vs)
            f.material_index = slot
            f.smooth = True
            for loop, (u, v) in zip(f.loops, ((0, 0), (1, 0), (1, 1), (0, 1))):
                loop[uvl].uv = (u, v)

        rnd = random.Random(13)
        for i in picks:
            p = Vector(centres[i])
            out = (p - Vector(crown))
            out.z *= 0.5
            out = out.normalized() if out.length > 1e-3 else Vector((0, 0, 1))
            if kind == 'oak':
                # A leafy twig sticking out of the crown: base inside, tip outwards, tilted up.
                size = rnd.uniform(0.45, 0.75)
                d = (out + Vector((rnd.uniform(-0.6, 0.6), rnd.uniform(-0.6, 0.6), rnd.uniform(-0.1, 0.7)))).normalized()
                side = d.cross(Vector((rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(-1, 1)))).normalized() * size * 0.5
                base = p - d * size * 0.3
                tip = base + d * size
                quad(base - side, base + side, tip + side, tip - side)
            else:
                # A strand hanging from the crown, swinging a little out, 1-2.2 m long.
                length = rnd.uniform(0.6, 1.2) if rnd.random() < 0.35 else rnd.uniform(1.3, 2.6)
                widthv = rnd.uniform(0.18, 0.26)
                yaw = rnd.uniform(0, 2 * math.pi)
                side = Vector((math.cos(yaw), math.sin(yaw), 0)) * widthv * 0.5
                top = p + out * 0.1
                bottom = top + Vector((out.x * 0.25, out.y * 0.25, -length))
                quad(bottom - side, bottom + side, top + side, top - side)
        bm.to_mesh(me)
        bm.free()
        me.update()
        tris = sum(len(p.vertices) - 2 for p in me.polygons)
        bpy.ops.object.select_all(action='DESELECT')
        obj.select_set(True)
        bpy.ops.export_scene.fbx(filepath=str(DIR / ('SM_%sLeafy.fbx' % name)), use_selection=True, object_types={'MESH'},
                                 apply_unit_scale=True, mesh_smooth_type='FACE', axis_forward='-Y', axis_up='Z', bake_space_transform=True)
        print('FOLIAGE_READY', name, 'cards', count, 'triangles', tris, flush=True)
        if RENDER:
            render(obj, name, kind)


def render(obj, name, kind):
    """A quick look in daylight: bark texture on the trunk, the leaf texture with alpha on the cards."""
    scene = bpy.context.scene
    bark = obj.data.materials[0]
    bark.use_nodes = True
    nt = bark.node_tree
    tex = nt.nodes.new('ShaderNodeTexImage')
    tex.image = bpy.data.images.load(str(DIR / ('T_%s_Base.png' % name)))
    nt.links.new(tex.outputs['Color'], nt.nodes['Principled BSDF'].inputs['Base Color'])
    leaves = obj.data.materials[-1]
    leaves.use_nodes = True
    nt = leaves.node_tree
    tex = nt.nodes.new('ShaderNodeTexImage')
    tex.image = bpy.data.images.load(str(DIR / ('T_%sLeaf.png' % ('Oak' if kind == 'oak' else 'Willow'))))
    bsdf = nt.nodes['Principled BSDF']
    nt.links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
    nt.links.new(tex.outputs['Alpha'], bsdf.inputs['Alpha'])
    leaves.blend_method = 'CLIP' if hasattr(leaves, 'blend_method') else None
    world = bpy.data.worlds.new('Sky')
    world.use_nodes = True
    world.node_tree.nodes['Background'].inputs['Color'].default_value = (0.55, 0.65, 0.8, 1)
    world.node_tree.nodes['Background'].inputs['Strength'].default_value = 1.0
    scene.world = world
    sun = bpy.data.objects.new('Sun', bpy.data.lights.new('Sun', 'SUN'))
    sun.data.energy = 3.5
    sun.rotation_euler = (math.radians(50), 0, math.radians(30))
    scene.collection.objects.link(sun)
    lo = Vector((min(v.co.x for v in obj.data.vertices), min(v.co.y for v in obj.data.vertices), 0))
    hi = Vector((max(v.co.x for v in obj.data.vertices), max(v.co.y for v in obj.data.vertices), max(v.co.z for v in obj.data.vertices)))
    cam = bpy.data.objects.new('Cam', bpy.data.cameras.new('Cam'))
    cam.data.clip_start, cam.data.clip_end = 0.01 * hi.z, 20 * hi.z
    scene.collection.objects.link(cam)
    target = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, hi.z * 0.45))
    cam.location = target + Vector((0, -hi.z * 1.9, hi.z * 0.05))
    cam.rotation_euler = (math.radians(90), 0, 0)
    scene.camera = cam
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = 24
    scene.cycles.device = 'CPU'
    scene.render.resolution_x, scene.render.resolution_y = 720, 720
    scene.render.filepath = 'C:/Fish/Saved/TreeRender_%s.png' % name
    bpy.ops.render.render(write_still=True)
    print('FOLIAGE render', scene.render.filepath, flush=True)


main()
