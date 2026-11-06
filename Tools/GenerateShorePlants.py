"""Shore and shallow-water plants for the lake level as FBX files (run with Blender 5.x).

    blender --background --factory-startup --python Tools/GenerateShorePlants.py

Real size in metres, origin at the ground (the bed under shallow water), colours in vertex colours and
the vertex alpha = height fraction, which M_GrassBlades uses for the wind sway (Tools/ImportShorePlants.py
imports them with it). Every blade, leaf and stem is a two-sided strip; normals point up, so a clump is
lit softly like grass.
  Reed_A, Reed_B - common reed (Phragmites): a clump of 2-3 m culms with alternate arching leaves and
                   nodding purple-brown panicles; a few last year's straw-coloured culms stand among them.
  Bulrush        - reedmace (Typha latifolia): grey-green twisted strap leaves to 2 m and stems with
                   a dark brown cattail and a thin spike above it.
  Sedge_A, Sedge_B - tussock sedge (Carex) for wet banks: dense arching blades, a few dark spikes.
Output: Tools/GeneratedPlants/SM_<Plant>.fbx
"""
import math
import os
import random
import bmesh
import bpy
from mathutils import Vector

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'GeneratedPlants')


def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def jitter(colour, rnd, amount):
    k = rnd.uniform(-amount, amount)
    return tuple(max(0.0, c + k) for c in colour)


class Builder:
    """A bmesh with a vertex-colour layer; alpha is the height of a vertex over the plant's top."""

    def __init__(self, top):
        self.bm = bmesh.new()
        self.col = self.bm.loops.layers.color.new('Col')
        self.top = top

    def strip(self, points, widths, sides, colours):
        """A strip along points; at each one half-width widths[i] towards +-sides[i] and colour colours[i]."""
        prev = None
        for p, w, s, c in zip(points, widths, sides, colours):
            row = (self.bm.verts.new(p - s * w), self.bm.verts.new(p + s * w), p, c)
            if prev:
                f = self.bm.faces.new([prev[0], prev[1], row[1], row[0]])
                for loop, (pt, cc) in zip(f.loops, ((prev[2], prev[3]), (prev[2], prev[3]), (row[2], row[3]), (row[2], row[3]))):
                    loop[self.col] = (*cc, min(1.0, max(0.0, pt.z / self.top)))
            prev = row

    def tube(self, base, axis, length, radius, colour, sides=8):
        """A closed little cylinder (a cattail, a spikelet)."""
        axis = axis.normalized()
        u = axis.orthogonal().normalized()
        v = axis.cross(u)
        rings = []
        for z in (0.0, length):
            centre = base + axis * z
            rings.append([self.bm.verts.new(centre + (u * math.cos(2 * math.pi * k / sides) + v * math.sin(2 * math.pi * k / sides)) * radius)
                          for k in range(sides)])
        faces = [[rings[0][k], rings[0][(k + 1) % sides], rings[1][(k + 1) % sides], rings[1][k]] for k in range(sides)]
        faces += [list(reversed(rings[0])), rings[1]]
        for vs in faces:
            f = self.bm.faces.new(vs)
            for loop in f.loops:
                loop[self.col] = (*colour, min(1.0, max(0.0, loop.vert.co.z / self.top)))


def horizontal(yaw):
    return Vector((math.cos(yaw), math.sin(yaw), 0.0))


def leaf(b, rnd, start, yaw, length, width, rise, droop, base_colour, tip_colour, segments=6, twist=0.0, shape='lance'):
    """A leaf from start, leaving at rise (degrees from vertical) and bending down by droop at its tip."""
    points, widths, sides, colours = [], [], [], []
    p = start.copy()
    step = length / segments
    for i in range(segments + 1):
        t = i / segments
        angle = math.radians(rise + droop * t ** 1.6)
        d = horizontal(yaw) * math.sin(angle) + Vector((0, 0, math.cos(angle)))
        points.append(p.copy())
        if shape == 'lance':       # narrow sheath, widest a fifth of the way, a long point
            w = width * min(1.0, 0.35 + t * 5.0) * (1.0 - t) ** 0.75
        else:                      # strap: even width, rounded at the tip
            w = width * (1.0 - t ** 6) + 0.0005
        widths.append(w + 0.0004)
        sides.append(horizontal(yaw + math.pi / 2 + twist * t))
        colours.append(mix(base_colour, tip_colour, t))
        p = p + d * step
    b.strip(points, widths, sides, colours)
    return p


def culm(b, base, height, lean_yaw, lean, radius, base_colour, top_colour, segments=8):
    """An upright stem: two crossed strips, curving a little towards lean_yaw. Returns its points."""
    pts = [base + horizontal(lean_yaw) * (lean * height * (i / segments) ** 2) + Vector((0, 0, height * i / segments))
           for i in range(segments + 1)]
    for side_yaw in (lean_yaw, lean_yaw + math.pi / 2):
        b.strip(pts, [radius * (1 - 0.55 * i / segments) for i in range(segments + 1)], [horizontal(side_yaw)] * len(pts),
                [mix(base_colour, top_colour, i / segments) for i in range(segments + 1)])
    return pts


def panicle(b, rnd, top, nod_yaw, size, colour, tip_colour, strands=14):
    """A loose nodding plume of strands hanging from the top of a culm."""
    for k in range(strands):
        start = top - Vector((0, 0, size * 0.55 * k / strands))
        yaw = nod_yaw + rnd.uniform(-1.1, 1.1)
        leaf(b, rnd, start, yaw, size * rnd.uniform(0.45, 0.9) * (1.0 - 0.35 * k / strands), rnd.uniform(0.004, 0.007),
             rnd.uniform(10, 35), rnd.uniform(70, 120), colour, tip_colour, segments=4, shape='strap')
    # the plume's axis, bent over the same way
    leaf(b, rnd, top - Vector((0, 0, size * 0.6)), nod_yaw, size * 0.85, 0.003, 5, 80, colour, tip_colour, segments=4, shape='strap')


def reed(seed, culms, radius, h_min, h_max, dead_share):
    rnd = random.Random(seed)
    b = Builder(h_max)
    wind_yaw = rnd.uniform(0, 2 * math.pi)       # the clump leans and nods mostly one way
    for _ in range(culms):
        a, r = rnd.uniform(0, 2 * math.pi), radius * math.sqrt(rnd.random())
        base = Vector((math.cos(a) * r, math.sin(a) * r, -0.05))
        dead = rnd.random() < dead_share
        h = rnd.uniform(h_min, h_max) * (0.85 if dead else 1.0)
        lean_yaw = wind_yaw + rnd.uniform(-0.8, 0.8)
        if dead:
            stem, stem_top = jitter((0.50, 0.43, 0.27), rnd, 0.04), jitter((0.62, 0.55, 0.38), rnd, 0.04)
        else:
            stem, stem_top = jitter((0.13, 0.20, 0.07), rnd, 0.02), jitter((0.30, 0.34, 0.14), rnd, 0.03)
        pts = culm(b, base, h, lean_yaw, rnd.uniform(0.02, 0.09), rnd.uniform(0.006, 0.009), stem, stem_top)
        leaves = rnd.randint(2, 4) if dead else rnd.randint(8, 12)
        yaw = rnd.uniform(0, 2 * math.pi)
        for k in range(leaves):
            t = 0.18 + 0.68 * k / max(1, leaves - 1)
            start = pts[0].lerp(pts[-1], t)
            yaw += math.pi + rnd.uniform(-0.5, 0.5)            # alternate sides
            if dead:
                leaf(b, rnd, start, yaw, rnd.uniform(0.15, 0.3), rnd.uniform(0.008, 0.014), rnd.uniform(50, 80), rnd.uniform(60, 110),
                     (0.52, 0.45, 0.30), (0.66, 0.58, 0.42), segments=4)
            else:
                leaf(b, rnd, start, yaw, rnd.uniform(0.25, 0.45) * (1.1 - 0.4 * t), rnd.uniform(0.014, 0.024), rnd.uniform(30, 55),
                     rnd.uniform(35, 85), jitter((0.12, 0.23, 0.07), rnd, 0.02), jitter((0.30, 0.40, 0.15), rnd, 0.03))
        if dead:
            panicle(b, rnd, pts[-1], lean_yaw, rnd.uniform(0.15, 0.25), (0.48, 0.42, 0.32), (0.66, 0.60, 0.50), strands=9)
        elif rnd.random() < 0.85:
            panicle(b, rnd, pts[-1], lean_yaw, rnd.uniform(0.2, 0.35), jitter((0.24, 0.14, 0.12), rnd, 0.03), (0.44, 0.34, 0.27))
    return b


def bulrush(seed):
    rnd = random.Random(seed)
    b = Builder(2.3)
    for _ in range(rnd.randint(4, 5)):                 # shoots, each a fan of strap leaves
        a, r = rnd.uniform(0, 2 * math.pi), 0.3 * math.sqrt(rnd.random())
        base = Vector((math.cos(a) * r, math.sin(a) * r, -0.05))
        fan = rnd.uniform(0, 2 * math.pi)
        for k in range(rnd.randint(6, 9)):
            yaw = fan + (k % 2) * math.pi + rnd.uniform(-0.35, 0.35)
            leaf(b, rnd, base + Vector((0, 0, 0.02 * k)), yaw, rnd.uniform(1.3, 2.1), rnd.uniform(0.007, 0.011), rnd.uniform(2, 10),
                 rnd.uniform(15, 60), jitter((0.14, 0.24, 0.10), rnd, 0.02), jitter((0.36, 0.44, 0.24), rnd, 0.03),
                 segments=8, twist=rnd.uniform(-1.2, 1.2), shape='strap')
    for _ in range(rnd.randint(3, 4)):                 # flowering stems with a cattail
        a, r = rnd.uniform(0, 2 * math.pi), 0.22 * math.sqrt(rnd.random())
        base = Vector((math.cos(a) * r, math.sin(a) * r, -0.05))
        h = rnd.uniform(1.6, 2.1)
        pts = culm(b, base, h, rnd.uniform(0, 2 * math.pi), rnd.uniform(0.0, 0.03), 0.006, (0.16, 0.24, 0.10), (0.30, 0.34, 0.16))
        axis = pts[-1] - pts[-2]
        cat_len = rnd.uniform(0.14, 0.2)
        b.tube(pts[-1] - axis.normalized() * (cat_len + 0.12), axis, cat_len, rnd.uniform(0.012, 0.016), jitter((0.17, 0.10, 0.06), rnd, 0.015))
        b.tube(pts[-1] - axis.normalized() * 0.11, axis, 0.13, 0.0035, (0.32, 0.27, 0.16), sides=5)
    return b


def sedge(seed, blades, radius, h_min, h_max, spikes):
    rnd = random.Random(seed)
    b = Builder(h_max)
    for _ in range(blades):
        a, r = rnd.uniform(0, 2 * math.pi), radius * math.sqrt(rnd.random())
        base = Vector((math.cos(a) * r, math.sin(a) * r, -0.02))
        yaw = a + rnd.uniform(-0.6, 0.6)               # blades lean out of the tussock
        tip = mix((0.30, 0.38, 0.15), (0.46, 0.42, 0.20), rnd.random() ** 3)   # some tips yellowing
        leaf(b, rnd, base, yaw, rnd.uniform(h_min, h_max), rnd.uniform(0.003, 0.0045), rnd.uniform(5, 25), rnd.uniform(40, 110),
             jitter((0.07, 0.14, 0.06), rnd, 0.015), jitter(tip, rnd, 0.02), segments=6, shape='strap')
    for _ in range(spikes):
        a, r = rnd.uniform(0, 2 * math.pi), radius * 0.6 * math.sqrt(rnd.random())
        base = Vector((math.cos(a) * r, math.sin(a) * r, -0.02))
        h = rnd.uniform(h_max * 0.7, h_max * 1.05)
        pts = culm(b, base, h, rnd.uniform(0, 2 * math.pi), rnd.uniform(0.05, 0.15), 0.0025, (0.12, 0.18, 0.07), (0.25, 0.28, 0.12), segments=5)
        axis = pts[-1] - pts[-2]
        for k in range(3):
            b.tube(pts[-1] - axis.normalized() * (0.045 * k + 0.04), axis + horizontal(rnd.uniform(0, 6.3)) * 0.02, 0.035, 0.004,
                   jitter((0.12, 0.09, 0.06), rnd, 0.02), sides=5)
    return b


PLANTS = {
    'Reed_A': lambda: reed(21, 26, 0.45, 1.9, 2.7, 0.15),
    'Reed_B': lambda: reed(22, 34, 0.6, 2.2, 3.0, 0.2),
    'Bulrush': lambda: bulrush(23),
    'Sedge_A': lambda: sedge(24, 110, 0.16, 0.45, 0.85, 6),
    'Sedge_B': lambda: sedge(25, 70, 0.12, 0.3, 0.6, 3),
}


def main():
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    only = [n for n in os.environ.get('QUIETLAKE_SHORE', '').split(',') if n]
    for name, build in PLANTS.items():
        if only and name not in only:
            continue
        for o in list(bpy.context.scene.objects):
            bpy.data.objects.remove(o, do_unlink=True)
        b = build()
        mesh = bpy.data.meshes.new('SM_' + name)
        b.bm.to_mesh(mesh)
        b.bm.free()
        for p in mesh.polygons:
            p.use_smooth = True
        mesh.normals_split_custom_set_from_vertices([(0.0, 0.0, 1.0)] * len(mesh.vertices))
        mesh.materials.append(bpy.data.materials.get('M_GrassBlades') or bpy.data.materials.new('M_GrassBlades'))
        obj = bpy.data.objects.new('SM_' + name, mesh)
        bpy.context.scene.collection.objects.link(obj)
        bpy.ops.object.select_all(action='DESELECT')
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        path = os.path.join(OUT, 'SM_%s.fbx' % name)
        bpy.ops.export_scene.fbx(filepath=path, use_selection=True, apply_unit_scale=True, object_types={'MESH'},
                                 mesh_smooth_type='OFF', colors_type='LINEAR', axis_forward='-Y', axis_up='Z',
                                 bake_space_transform=True)
        size = obj.dimensions
        print('SHORE_PLANT %s %d verts %d faces %.2f x %.2f x %.2f m' % (name, len(mesh.vertices), len(mesh.polygons), size.x, size.y, size.z))


main()
