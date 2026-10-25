"""Generate the four lake fish as FBX files (run with Blender 5.x).

    blender --background --factory-startup --python Tools/GenerateFishModels.py

Each fish is built procedurally: a lofted body from per-species proportions, flat fins,
eyes and colouring in vertex colours. Body length runs along +X (snout at X = 0).
UV0.x is the normalised position along the fish (0 = snout, 1 = tail tip); the UE material
uses it to bend the body more towards the tail when swimming.
Output: Tools/GeneratedFish/SM_Fish_<Species>.fbx, in metres (UE imports them in cm).
"""
import math
import os
import bmesh
import bpy

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'GeneratedFish')


def lerp(a, b, t):
    return a + (b - a) * t


def mix(c1, c2, t):
    return tuple(lerp(a, b, t) for a, b in zip(c1, c2))


def smooth(e0, e1, x):
    t = max(0.0, min(1.0, (x - e0) / (e1 - e0)))
    return t * t * (3 - 2 * t)


def curve(points, t):
    """Piecewise smooth interpolation through (t, value) points."""
    for (t0, v0), (t1, v1) in zip(points, points[1:]):
        if t <= t1:
            k = (t - t0) / (t1 - t0) if t1 > t0 else 0
            k = k * k * (3 - 2 * k)
            return lerp(v0, v1, k)
    return points[-1][1]


def hash2(x, y):
    v = math.sin(x * 127.1 + y * 311.7) * 43758.5453
    return v - math.floor(v)


# Proportions are relative to total length L (snout to tail tip). Body ends at 'body' (start of tail fin).
SPECIES = {
    'Perch': dict(
        L=0.26, body=0.84,
        height=[(0, 0.0), (0.06, 0.07), (0.25, 0.13), (0.4, 0.135), (0.7, 0.08), (0.84, 0.045)],
        width=[(0, 0.0), (0.06, 0.04), (0.3, 0.065), (0.6, 0.05), (0.84, 0.018)],
        center=[(0, -0.01), (0.3, 0.012), (0.84, 0.0)],
        back=(0.10, 0.17, 0.05), side=(0.48, 0.52, 0.18), belly=(0.82, 0.80, 0.62),
        fin=(0.78, 0.22, 0.08), stripes=6,
        fins=[('dorsal', 0.22, 0.48, 0.09, (0.30, 0.32, 0.20)), ('dorsal', 0.50, 0.72, 0.06, (0.55, 0.50, 0.22)),
              ('anal', 0.60, 0.72, 0.045, None), ('pelvic', 0.26, 0.34, 0.05, None), ('pectoral', 0.20, 0.27, 0.06, None)],
        tail_span=0.11, fork=0.35),
    'Roach': dict(
        L=0.22, body=0.82,
        height=[(0, 0.0), (0.06, 0.06), (0.3, 0.12), (0.45, 0.12), (0.7, 0.07), (0.82, 0.04)],
        width=[(0, 0.0), (0.06, 0.035), (0.35, 0.05), (0.82, 0.015)],
        center=[(0, 0.0), (0.4, 0.008), (0.82, 0.0)],
        back=(0.09, 0.14, 0.18), side=(0.70, 0.72, 0.72), belly=(0.92, 0.92, 0.90),
        fin=(0.72, 0.18, 0.10), stripes=0,
        fins=[('dorsal', 0.40, 0.55, 0.07, None), ('anal', 0.60, 0.72, 0.05, None),
              ('pelvic', 0.40, 0.48, 0.045, None), ('pectoral', 0.20, 0.27, 0.05, None)],
        tail_span=0.10, fork=0.4),
    'Crucian': dict(
        L=0.20, body=0.82,
        height=[(0, 0.0), (0.07, 0.10), (0.3, 0.19), (0.5, 0.19), (0.72, 0.10), (0.82, 0.06)],
        width=[(0, 0.0), (0.07, 0.04), (0.4, 0.07), (0.82, 0.02)],
        center=[(0, -0.01), (0.35, 0.015), (0.82, 0.0)],
        back=(0.22, 0.16, 0.05), side=(0.66, 0.48, 0.16), belly=(0.86, 0.72, 0.42),
        fin=(0.40, 0.25, 0.10), stripes=0,
        fins=[('dorsal', 0.32, 0.68, 0.06, None), ('anal', 0.62, 0.72, 0.05, None),
              ('pelvic', 0.38, 0.46, 0.05, None), ('pectoral', 0.20, 0.28, 0.05, None)],
        tail_span=0.10, fork=0.2),
    'Pike': dict(
        L=0.62, body=0.86,
        height=[(0, 0.0), (0.03, 0.025), (0.12, 0.05), (0.4, 0.075), (0.7, 0.07), (0.86, 0.035)],
        width=[(0, 0.0), (0.03, 0.03), (0.12, 0.045), (0.5, 0.05), (0.86, 0.015)],
        center=[(0, -0.005), (0.5, 0.0), (0.86, 0.0)],
        back=(0.08, 0.13, 0.05), side=(0.28, 0.36, 0.12), belly=(0.85, 0.83, 0.70),
        fin=(0.42, 0.30, 0.10), stripes=0, spots=True,
        fins=[('dorsal', 0.66, 0.80, 0.06, None), ('anal', 0.68, 0.80, 0.05, None),
              ('pelvic', 0.48, 0.55, 0.04, None), ('pectoral', 0.16, 0.22, 0.045, None)],
        tail_span=0.09, fork=0.3),
}


def body_colour(s, t, theta, z_norm):
    up = (math.sin(theta) + 1) * 0.5                       # 0 belly .. 1 back
    c = mix(s['belly'], s['side'], smooth(0.12, 0.45, up))
    c = mix(c, s['back'], smooth(0.6, 0.95, up))
    if s.get('stripes'):
        band = 0.5 + 0.5 * math.sin((t - 0.18) / (s['body'] - 0.25) * math.pi * 2 * s['stripes'] / 2)
        dark = smooth(0.55, 0.85, band) * smooth(0.18, 0.25, t) * (1 - smooth(0.72, 0.8, t)) * smooth(0.3, 0.55, up)
        c = mix(c, (0.05, 0.07, 0.03), dark * 0.85)
    if s.get('spots'):
        cell = hash2(math.floor(t * 70), math.floor(theta * 6))
        if cell > 0.72 and 0.25 < up < 0.8:
            c = mix(c, (0.72, 0.70, 0.40), 0.6)
    # A darker lateral line and slight speckle break up the flat look.
    c = mix(c, tuple(x * 0.7 for x in c), smooth(0.04, 0.0, abs(up - 0.62)) * 0.5)
    k = 0.92 + 0.08 * hash2(t * 400, theta * 17)
    return tuple(x * k for x in c)


def build_fish(name, s):
    L = s['L']
    mesh = bpy.data.meshes.new('SM_Fish_' + name)
    bm = bmesh.new()
    col = bm.loops.layers.color.new('Col')
    uv = bm.loops.layers.uv.new('UVMap')

    def add_face(verts, colours, us):
        f = bm.faces.new(verts)
        for loop, c, u in zip(f.loops, colours, us):
            loop[col] = (*c, 1.0)
            loop[uv].uv = (u, 0.5)
        return f

    # --- body: rings of an ellipse along X ---
    rings, segs = 56, 28
    grid = []
    for i in range(rings + 1):
        t = s['body'] * (i / rings) ** 1.15            # denser near the head
        h = curve(s['height'], t) * L          # profile values are half-height / half-width
        w = curve(s['width'], t) * L
        cz = curve(s['center'], t) * L
        ring = []
        for j in range(segs):
            th = 2 * math.pi * j / segs
            ring.append((bm.verts.new((t * L, math.cos(th) * w, cz + math.sin(th) * h)), t, th))
        grid.append(ring)
    for i in range(rings):
        for j in range(segs):
            a, b = grid[i][j], grid[i][(j + 1) % segs]
            c, d = grid[i + 1][(j + 1) % segs], grid[i + 1][j]
            quad = [a, b, c, d]
            add_face([q[0] for q in quad], [body_colour(s, q[1], q[2], 0) for q in quad], [q[1] for q in quad])
    # close the tail end of the body
    end = grid[-1]
    centre = bm.verts.new((s['body'] * L, 0, curve(s['center'], s['body']) * L))
    for j in range(segs):
        a, b = end[j], end[(j + 1) % segs]
        add_face([a[0], centre, b[0]], [s['fin']] * 3, [s['body']] * 3)

    def fin(points, colour_root, colour_tip, mirror_y=None):
        """Flat fin from a polygon outline: points are (x, y, z) in metres, first point is the root."""
        for side in ([1, -1] if mirror_y is not None else [1]):
            vs = [bm.verts.new((p[0], p[1] * side if mirror_y is not None else p[1], p[2])) for p in points]
            cs = [colour_root] + [colour_tip] * (len(points) - 1)
            us = [p[0] / L for p in points]
            if side == -1:
                vs, cs, us = vs[::-1], cs[::-1], us[::-1]
            add_face(vs, cs, us)

    # --- tail fin ---
    tb = s['body'] * L
    span = s['tail_span'] * L
    fork_x = tb + (L - tb) * s['fork']
    tail_col = s['fin']
    fin([(tb, 0, span * 0.18), (L, 0, span), (fork_x, 0, 0), (L, 0, -span), (tb, 0, -span * 0.18)],
        mix(tail_col, s['back'], 0.4), tail_col)

    # --- other fins ---
    for kind, t0, t1, size, colour in s['fins']:
        c = colour or s['fin']
        x0, x1 = t0 * L, t1 * L
        tm = (t0 + t1) * 0.5
        if kind == 'dorsal':
            z0 = curve(s['center'], t0) * L + curve(s['height'], t0) * L * 0.94
            z1 = curve(s['center'], t1) * L + curve(s['height'], t1) * L * 0.94
            fin([(x0, 0, z0), (x0 + (x1 - x0) * 0.25, 0, z0 + size * L), (x1, 0, z1 + size * L * 0.4), (x1, 0, z1)],
                mix(c, s['back'], 0.3), c)
        elif kind == 'anal':
            z0 = curve(s['center'], t0) * L - curve(s['height'], t0) * L * 0.94
            z1 = curve(s['center'], t1) * L - curve(s['height'], t1) * L * 0.94
            fin([(x0, 0, z0), (x0 + (x1 - x0) * 0.3, 0, z0 - size * L), (x1, 0, z1 - size * L * 0.5), (x1, 0, z1)], c, c)
        else:
            # paired fins: angled out and down from the lower side
            zb = curve(s['center'], tm) * L - curve(s['height'], tm) * L * (0.3 if kind == 'pectoral' else 0.84)
            yb = curve(s['width'], tm) * L * 0.9
            fin([(x0, yb, zb), (x0 + size * L * 0.9, yb + size * L * 0.55, zb - size * L * 0.45), (x1, yb * 0.9, zb)],
                c, c, mirror_y=True)

    # --- eyes: small dark discs with a golden ring ---
    te = 0.075 if name != 'Pike' else 0.09
    xe = te * L
    ze = curve(s['center'], te) * L + curve(s['height'], te) * L * 0.36
    ye = curve(s['width'], te) * L + 0.0005
    re = L * (0.022 if name != 'Pike' else 0.012)
    for side in (1, -1):
        centre_v = bm.verts.new((xe, ye * side, ze))
        ring = [bm.verts.new((xe + math.cos(a) * re, ye * side, ze + math.sin(a) * re)) for a in
                [2 * math.pi * k / 12 for k in range(12)]]
        for k in range(12):
            tri = [centre_v, ring[k], ring[(k + 1) % 12]]
            if side == -1:
                tri = tri[::-1]
            add_face(tri, [(0.01, 0.01, 0.01), (0.55, 0.42, 0.12), (0.55, 0.42, 0.12)], [te] * 3)

    bm.normal_update()
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new('SM_Fish_' + name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    for poly in mesh.polygons:
        poly.use_smooth = True
    mat = bpy.data.materials.get('M_Fish') or bpy.data.materials.new('M_Fish')
    mesh.materials.append(mat)
    return obj


def main():
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    for name, spec in SPECIES.items():
        for o in list(bpy.context.scene.objects):
            bpy.data.objects.remove(o, do_unlink=True)
        obj = build_fish(name, spec)
        bpy.ops.object.select_all(action='DESELECT')
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        path = os.path.join(OUT, 'SM_Fish_%s.fbx' % name)
        bpy.ops.export_scene.fbx(filepath=path, use_selection=True, apply_unit_scale=True,
                                 object_types={'MESH'}, mesh_smooth_type='FACE', colors_type='LINEAR',
                                 axis_forward='-Y', axis_up='Z', bake_space_transform=True)
        print('FISH_EXPORTED', name, len(obj.data.vertices), 'verts', path)


main()
