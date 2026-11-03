"""Generate water plants as FBX files (run with Blender 5.x).

    blender --background --factory-startup --python Tools/GeneratePlantModels.py

Real size in metres, colours in vertex colours (M_Plant in Unreal). Origin at the water surface,
so the game places every part at the waterline:
  LilyPad_A/B/C - floating leaves of the white water lily (Nymphaea): fresh, ageing with a curled
                  edge, young reddish; a notch runs from the centre to the rim.
  LilyFlower    - an open white water lily lying on the water.
  LilyYellow    - yellow water lily (Nuphar lutea): a closed yellow cup on a stalk above the water.
Ground plants (origin at the ground; vertex alpha = height fraction, used for wind sway by M_GrassBlades):
  Grass_Short   - a low tuft of meadow grass.
  Grass_Tall    - tall meadow grass with seed heads.
  Sedge         - dark, arching sedge for the damp banks.
  Meadow_Flowers- grass with chamomile and buttercups.
  Grass_Waist   - dense waist-high bank grass (0.7-1.1 m) with seed heads, for the strip along the water.
  Meadowsweet   - tall meadowsweet: stems to 1.3 m with creamy flower clusters, among the bank grass.
Output: Tools/GeneratedPlants/SM_<Part>.fbx
"""
import math
import os
import random
import bmesh
import bpy
from mathutils import Vector

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'GeneratedPlants')


def new_bm():
    bm = bmesh.new()
    return bm, bm.loops.layers.color.new('Col')


def face(bm, col, verts, colour, alphas=None):
    f = bm.faces.new(verts)
    for i, loop in enumerate(f.loops):
        loop[col] = (*colour, alphas[i] if alphas else 1.0)
    return f


def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def pad(seed, radius, top, rim, under, curl=0.0, tear=0.0):
    """A round floating leaf with a notch; slightly cupped, the rim curled up by `curl`."""
    rnd = random.Random(seed)
    bm, col = new_bm()
    rings, segs = 3, 22                            # light: lily beds hold tens of thousands of leaves
    notch = 0.16                                   # half-angle of the slit (rad)
    wob = [rnd.uniform(-1, 1) for _ in range(6)]
    grid = []
    for i in range(rings + 1):
        t = i / rings
        row = []
        for j in range(segs + 1):
            a = notch + (2 * math.pi - 2 * notch) * j / segs
            edge = 1 + 0.04 * sum(w * math.sin((k + 2) * a + k) for k, w in enumerate(wob))
            if tear and 2.2 < a < 2.6:
                edge -= tear * math.sin((a - 2.2) / 0.4 * math.pi)
            r = radius * t * edge
            z = 0.002 * t + curl * max(0.0, t - 0.75) ** 2 * 16 * (0.6 + 0.4 * math.sin(a * 3 + seed))
            row.append(bm.verts.new(Vector((math.cos(a) * r, math.sin(a) * r, z))))
        grid.append(row)
    for i in range(rings):
        for j in range(segs):
            t = (i + 0.5) / rings
            c = mix(top, rim, t ** 2)
            # Faint radial veins: every few segments slightly lighter.
            if j % 3 == 0:
                c = mix(c, (0.35, 0.5, 0.2), 0.25)
            if i == 0:
                face(bm, col, [grid[0][j], grid[1][j], grid[1][j + 1]], c)
            else:
                face(bm, col, [grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]], c)
    # Underside, a few millimetres lower and reddish, so curled edges show the leaf's back.
    under_rows = [[bm.verts.new(v.co - Vector((0, 0, 0.003))) for v in row] for row in grid]
    for i in range(rings):
        for j in range(segs):
            if i == 0:
                face(bm, col, [under_rows[0][j], under_rows[1][j + 1], under_rows[1][j]], under)
            else:
                face(bm, col, [under_rows[i][j], under_rows[i][j + 1], under_rows[i + 1][j + 1], under_rows[i + 1][j]], under)
    return bm


def petal(bm, col, base, direction, up, length, width, lift, colour, tip_colour):
    """A boat-shaped petal from base along direction, its tip lifted by `lift`."""
    side = direction.cross(up).normalized()
    rows = []
    for i in range(6):
        t = i / 5
        w = width * math.sin(math.pi * min(0.98, 0.15 + t * 0.85)) * (1 - 0.6 * t * t)
        centre = base + direction * length * t + up * (lift * t * t)
        rows.append([centre + side * w * s + up * (abs(s) * width * 0.35) for s in (-1.0, -0.5, 0.0, 0.5, 1.0)])
    verts = [[bm.verts.new(p) for p in row] for row in rows]
    for i in range(5):
        c = mix(colour, tip_colour, i / 5)
        for j in range(4):
            face(bm, col, [verts[i][j], verts[i][j + 1], verts[i + 1][j + 1], verts[i + 1][j]], c)


def flower():
    bm, col = new_bm()
    up = Vector((0, 0, 1))
    white, cream, green = (0.92, 0.92, 0.88), (0.98, 0.97, 0.93), (0.25, 0.4, 0.15)
    # Green sepals lying on the water, then two rings of white petals, the inner ones more upright.
    for k in range(4):
        a = 2 * math.pi * k / 4 + 0.4
        petal(bm, col, Vector((0, 0, 0.004)), Vector((math.cos(a), math.sin(a), 0)), up, 0.05, 0.014, 0.012, green, (0.3, 0.32, 0.18))
    for ring, (count, length, width, lift, z) in enumerate([(12, 0.055, 0.012, 0.03, 0.008), (10, 0.042, 0.010, 0.045, 0.014)]):
        for k in range(count):
            a = 2 * math.pi * (k + 0.5 * ring) / count
            petal(bm, col, Vector((0, 0, z)), Vector((math.cos(a), math.sin(a), 0.25 + ring * 0.35)).normalized(), up,
                  length, width, lift, white, cream)
    # Yellow stamens in the centre.
    for k in range(28):
        a = k * 2.399
        r = 0.003 + 0.011 * math.sqrt(k / 28)
        base = Vector((math.cos(a) * r, math.sin(a) * r, 0.012))
        tip = base + Vector((math.cos(a) * 0.004, math.sin(a) * 0.004, 0.012))
        s = Vector((-math.sin(a), math.cos(a), 0)) * 0.0012
        face(bm, col, [bm.verts.new(base - s), bm.verts.new(base + s), bm.verts.new(tip + s), bm.verts.new(tip - s)], (0.95, 0.72, 0.1))
    return bm


def yellow():
    bm, col = new_bm()
    # Stalk rising out of the water, slightly bent.
    pts = [Vector((0.004 * math.sin(t * 2), 0, -0.04 + 0.12 * t)) for t in [i / 6 for i in range(7)]]
    for a, b in zip(pts, pts[1:]):
        ring_a = [bm.verts.new(a + Vector((math.cos(q) * 0.0035, math.sin(q) * 0.0035, 0))) for q in [2 * math.pi * k / 6 for k in range(6)]]
        ring_b = [bm.verts.new(b + Vector((math.cos(q) * 0.0035, math.sin(q) * 0.0035, 0))) for q in [2 * math.pi * k / 6 for k in range(6)]]
        for k in range(6):
            face(bm, col, [ring_a[k], ring_a[(k + 1) % 6], ring_b[(k + 1) % 6], ring_b[k]], (0.32, 0.42, 0.16))
    # Five thick, rounded sepals form the closed yellow cup.
    top = pts[-1]
    up = Vector((0, 0, 1))
    for k in range(5):
        a = 2 * math.pi * k / 5
        d = Vector((math.cos(a), math.sin(a), 1.6)).normalized()
        petal(bm, col, top, d, up, 0.032, 0.014, -0.004, (0.95, 0.75, 0.08), (0.98, 0.82, 0.18))
    return bm


def blade(bm, col, rnd, base, height, width, lean, base_colour, tip_colour, segments=4):
    """A tapered, curving grass blade; vertex alpha carries the height fraction for the wind."""
    yaw = rnd.uniform(0, 2 * math.pi)
    d = Vector((math.cos(yaw), math.sin(yaw), 0))
    side = Vector((-d.y, d.x, 0))
    prev = None
    for i in range(segments + 1):
        t = i / segments
        centre = base + d * (lean * height * t * t) + Vector((0, 0, height * t * (1 - 0.25 * lean * t)))
        w = width * (1 - t) ** 0.8 + 0.0004
        row = (bm.verts.new(centre - side * w), bm.verts.new(centre + side * w), t)
        if prev:
            c = mix(base_colour, tip_colour, (prev[2] + t) * 0.5)
            face(bm, col, [prev[0], prev[1], row[1], row[0]], c, [prev[2], prev[2], t, t])
        prev = row


def tuft(seed, count, radius, h_min, h_max, width, lean, base_colour, tip_colour, extra=None, segments=4):
    rnd = random.Random(seed)
    bm, col = new_bm()
    for _ in range(count):
        a, r = rnd.uniform(0, 2 * math.pi), radius * math.sqrt(rnd.random())
        base = Vector((math.cos(a) * r, math.sin(a) * r, -0.01))
        tint = rnd.uniform(-0.03, 0.03)
        blade(bm, col, rnd, base, rnd.uniform(h_min, h_max), width * rnd.uniform(0.7, 1.2), lean * rnd.uniform(0.5, 1.3),
              tuple(max(0, c + tint) for c in base_colour), tuple(max(0, c + tint) for c in tip_colour), segments)
    if extra:
        extra(bm, col, rnd)
    return bm


def seed_heads(bm, col, rnd, count=9, h_min=0.55, h_max=0.8, spread=0.12):
    """Thin stems with loose panicles above the tall grass."""
    for _ in range(count):
        a, r = rnd.uniform(0, 2 * math.pi), spread * math.sqrt(rnd.random())
        base = Vector((math.cos(a) * r, math.sin(a) * r, 0))
        h = rnd.uniform(h_min, h_max)
        blade(bm, col, rnd, base, h, 0.0012, 0.15, (0.30, 0.34, 0.12), (0.45, 0.40, 0.22), 5)
        top = base + Vector((0, 0, h))
        for k in range(6):
            q = rnd.uniform(0, 2 * math.pi)
            tip = top + Vector((math.cos(q) * 0.015, math.sin(q) * 0.015, -0.025 - 0.015 * k / 6))
            s = Vector((-math.sin(q), math.cos(q), 0)) * 0.0012
            p0 = top - Vector((0, 0, 0.012 * k))
            face(bm, col, [bm.verts.new(p0 - s), bm.verts.new(p0 + s), bm.verts.new(tip + s), bm.verts.new(tip - s)],
                 (0.42, 0.38, 0.24), [0.95, 0.95, 1.0, 1.0])


def flowers(bm, col, rnd):
    """Chamomile (white with a yellow eye) and buttercups (yellow) on thin stems."""
    for k in range(11):
        a, r = rnd.uniform(0, 2 * math.pi), 0.14 * math.sqrt(rnd.random())
        base = Vector((math.cos(a) * r, math.sin(a) * r, 0))
        h = rnd.uniform(0.25, 0.45)
        blade(bm, col, rnd, base, h, 0.0012, 0.1, (0.18, 0.30, 0.08), (0.25, 0.38, 0.10), 3)
        top = base + Vector((0, 0, h))
        chamomile = k % 3 != 0
        petals, length = (12, 0.012) if chamomile else (5, 0.007)
        petal_colour = (0.92, 0.92, 0.88) if chamomile else (0.95, 0.75, 0.05)
        for j in range(petals):
            q = 2 * math.pi * j / petals
            d = Vector((math.cos(q), math.sin(q), 0.15))
            s = Vector((-math.sin(q), math.cos(q), 0)) * (0.002 if chamomile else 0.0035)
            face(bm, col, [bm.verts.new(top - s), bm.verts.new(top + s), bm.verts.new(top + d * length + s), bm.verts.new(top + d * length - s)],
                 petal_colour, [1.0] * 4)
        if chamomile:
            e = [bm.verts.new(top + Vector((math.cos(q) * 0.004, math.sin(q) * 0.004, 0.002))) for q in [2 * math.pi * j / 6 for j in range(6)]]
            face(bm, col, e, (0.95, 0.72, 0.1), [1.0] * 6)


def meadowsweet(bm, col, rnd):
    """Reddish stems with pinnate leaves and dense creamy flower clusters on top."""
    for _ in range(6):
        a, r = rnd.uniform(0, 2 * math.pi), 0.18 * math.sqrt(rnd.random())
        base = Vector((math.cos(a) * r, math.sin(a) * r, 0))
        h = rnd.uniform(0.95, 1.3)
        blade(bm, col, rnd, base, h, 0.004, 0.08, (0.25, 0.12, 0.08), (0.3, 0.35, 0.12), 6)
        top = base + Vector((0, 0, h * 0.98))
        for k in range(3):                       # a few leaves up the stem
            z = h * (0.3 + 0.2 * k)
            q = rnd.uniform(0, 2 * math.pi)
            d = Vector((math.cos(q), math.sin(q), 0.3))
            s = Vector((-math.sin(q), math.cos(q), 0)) * 0.03
            p0 = base + Vector((0, 0, z))
            face(bm, col, [bm.verts.new(p0 - s * 0.3), bm.verts.new(p0 + s * 0.3), bm.verts.new(p0 + d * 0.14 + s), bm.verts.new(p0 + d * 0.14 - s)],
                 (0.12, 0.24, 0.06), [z / h] * 4)
        for k in range(40):                      # the flower cluster
            q, rr = rnd.uniform(0, 2 * math.pi), 0.06 * math.sqrt(rnd.random())
            c = top + Vector((math.cos(q) * rr, math.sin(q) * rr, rnd.uniform(-0.03, 0.05)))
            s1, s2 = Vector((0.006, 0, 0)), Vector((0, 0.006, 0.004))
            face(bm, col, [bm.verts.new(c - s1), bm.verts.new(c + s2), bm.verts.new(c + s1), bm.verts.new(c - s2)],
                 (0.92, 0.88, 0.72), [1.0] * 4)


PARTS = {
    'LilyPad_A': lambda: pad(1, 0.13, (0.10, 0.26, 0.06), (0.16, 0.32, 0.08), (0.32, 0.12, 0.10)),
    'LilyPad_B': lambda: pad(2, 0.14, (0.14, 0.26, 0.06), (0.42, 0.36, 0.10), (0.30, 0.14, 0.10), curl=0.02, tear=0.25),
    'LilyPad_C': lambda: pad(3, 0.09, (0.16, 0.22, 0.07), (0.34, 0.14, 0.08), (0.36, 0.10, 0.10), curl=0.006),
    'LilyFlower': flower,
    'LilyYellow': yellow,
    'Grass_Short': lambda: tuft(11, 90, 0.3, 0.1, 0.24, 0.005, 0.35, (0.13, 0.22, 0.05), (0.42, 0.52, 0.17), segments=2),
    'Grass_Tall': lambda: tuft(12, 80, 0.32, 0.3, 0.55, 0.005, 0.45, (0.12, 0.21, 0.05), (0.48, 0.52, 0.2), seed_heads, segments=3),
    'Sedge': lambda: tuft(13, 70, 0.22, 0.4, 0.75, 0.006, 0.8, (0.08, 0.17, 0.06), (0.30, 0.40, 0.14)),
    'Meadow_Flowers': lambda: tuft(14, 60, 0.3, 0.1, 0.28, 0.005, 0.35, (0.13, 0.22, 0.05), (0.42, 0.52, 0.17), flowers, segments=2),
    'Grass_Waist': lambda: tuft(15, 110, 0.36, 0.6, 1.05, 0.007, 0.5, (0.11, 0.19, 0.05), (0.44, 0.48, 0.19),
                                lambda bm, col, rnd: seed_heads(bm, col, rnd, 10, 0.9, 1.2, 0.3), segments=3),
    'Meadowsweet': lambda: tuft(16, 70, 0.3, 0.4, 0.8, 0.006, 0.45, (0.1, 0.18, 0.05), (0.36, 0.42, 0.15), meadowsweet),
}


def main():
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    for name, build in PARTS.items():
        for o in list(bpy.context.scene.objects):
            bpy.data.objects.remove(o, do_unlink=True)
        bm = build()
        mesh = bpy.data.meshes.new('SM_' + name)
        bm.to_mesh(mesh)
        bm.free()
        for p in mesh.polygons:
            p.use_smooth = True
        ground_plant = not name.startswith('Lily')
        if ground_plant:
            mesh.normals_split_custom_set_from_vertices([(0.0, 0.0, 1.0)] * len(mesh.vertices))
        mat_name = 'M_Plant' if name.startswith('Lily') else 'M_GrassBlades'
        mesh.materials.append(bpy.data.materials.get(mat_name) or bpy.data.materials.new(mat_name))
        obj = bpy.data.objects.new('SM_' + name, mesh)
        bpy.context.scene.collection.objects.link(obj)
        bpy.ops.object.select_all(action='DESELECT')
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        path = os.path.join(OUT, 'SM_%s.fbx' % name)
        bpy.ops.export_scene.fbx(filepath=path, use_selection=True, apply_unit_scale=True, object_types={'MESH'},
                                 mesh_smooth_type='OFF' if ground_plant else 'FACE', colors_type='LINEAR', axis_forward='-Y', axis_up='Z',
                                 bake_space_transform=True)
        print('PLANT_EXPORTED', name, len(mesh.vertices), path)


main()
