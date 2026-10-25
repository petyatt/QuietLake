"""Generate fishing tackle as FBX files (run with Blender 5.x).

    blender --background --factory-startup --python Tools/GenerateTackleModels.py

All parts are built in real size (metres) with colours in vertex colours.
Vertex alpha marks metal (alpha 0 = metallic) for M_Tackle in Unreal.
Pivot conventions used by the game code (LakeRod.cpp):
  bobber  - origin at the waterline, antenna up (+Z), line eye at the keel tip (Z = -BOBBER_KEEL)
  hook    - origin at the eye (line attachment), shank hanging down (-Z), point towards +X
  baits   - same frame as the hook, so they are attached with the hook's transform
  reel    - origin at the reel foot (where it clamps to the rod), body below (-Z)
  handle  - cork grip along +X from the butt (X = 0)
  stand   - forked stick, origin at the ground, fork at the top
Output: Tools/GeneratedTackle/SM_<Part>.fbx
"""
import math
import os
import bmesh
import bpy
from mathutils import Vector

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'GeneratedTackle')
BOBBER_KEEL = 0.035


def new_bm():
    bm = bmesh.new()
    return bm, bm.loops.layers.color.new('Col')


def face(bm, col, verts, colour, alpha=1.0):
    f = bm.faces.new(verts)
    for loop in f.loops:
        loop[col] = (*colour, alpha)
    return f


def lathe(bm, col, profile, segments=20, colour_fn=None, alpha=1.0, offset=Vector((0, 0, 0)), alpha_fn=None):
    """Revolve (radius, z) points around Z. colour_fn(z) -> rgb, alpha_fn(z) -> vertex alpha."""
    rings = []
    for r, z in profile:
        ring = []
        for j in range(segments):
            a = 2 * math.pi * j / segments
            ring.append(bm.verts.new(offset + Vector((math.cos(a) * r, math.sin(a) * r, z))))
        rings.append((ring, z))
    for (r0, z0), (r1, z1) in zip(rings, rings[1:]):
        c = colour_fn((z0 + z1) * 0.5) if colour_fn else (0.5, 0.5, 0.5)
        a = alpha_fn((z0 + z1) * 0.5) if alpha_fn else alpha
        for j in range(segments):
            face(bm, col, [r0[j], r0[(j + 1) % segments], r1[(j + 1) % segments], r1[j]], c, a)
    return rings


def tube(bm, col, points, radius, colour, segments=8, alpha=1.0, radius_fn=None, colour_fn=None):
    """Tube along a polyline of Vectors; radius_fn(t) and colour_fn(t) with t in 0..1."""
    rings = []
    n = len(points)
    for i, p in enumerate(points):
        t = i / (n - 1)
        d = (points[min(i + 1, n - 1)] - points[max(i - 1, 0)]).normalized()
        side = d.cross(Vector((0, 0, 1)))
        if side.length < 1e-4:
            side = d.cross(Vector((1, 0, 0)))
        side.normalize()
        up = side.cross(d).normalized()
        r = radius * (radius_fn(t) if radius_fn else 1.0)
        rings.append([bm.verts.new(p + (side * math.cos(a) + up * math.sin(a)) * r)
                      for a in [2 * math.pi * k / segments for k in range(segments)]])
    for i in range(n - 1):
        c = colour_fn((i + 0.5) / (n - 1)) if colour_fn else colour
        for k in range(segments):
            face(bm, col, [rings[i][k], rings[i][(k + 1) % segments], rings[i + 1][(k + 1) % segments], rings[i + 1][k]], c, alpha)
    # caps
    for ring, rev in ((rings[0], True), (rings[-1], False)):
        c = colour_fn(0.0 if rev else 1.0) if colour_fn else colour
        face(bm, col, ring[::-1] if rev else ring, c, alpha)


def blob(bm, col, centre, radii, colour, seed=0.0, segments=12, rings=8, lumpiness=0.18):
    """Irregular ellipsoid (bread crumb, lead shot)."""
    grid = []
    for i in range(rings + 1):
        v = math.pi * i / rings
        row = []
        for j in range(segments):
            u = 2 * math.pi * j / segments
            n = 1 + lumpiness * math.sin(u * 3 + seed) * math.sin(v * 2 + seed * 1.7)
            row.append(bm.verts.new(centre + Vector((math.sin(v) * math.cos(u) * radii[0] * n,
                                                     math.sin(v) * math.sin(u) * radii[1] * n,
                                                     math.cos(v) * radii[2] * n))))
        grid.append(row)
    for i in range(rings):
        for j in range(segments):
            quad = [grid[i][j], grid[i][(j + 1) % segments], grid[i + 1][(j + 1) % segments], grid[i + 1][j]]
            if i == 0:
                quad = [grid[0][j], grid[1][(j + 1) % segments], grid[1][j]]
            elif i == rings - 1:
                quad = [grid[i][j], grid[i][(j + 1) % segments], grid[i + 1][j]]
            try:
                face(bm, col, quad, colour)
            except ValueError:
                pass


# --- parts --------------------------------------------------------------------------------

def bobber():
    bm, col = new_bm()
    # Teardrop body from keel tip (bottom) to the antenna base; waterline at Z = 0.
    k = BOBBER_KEEL
    profile = [(0.0006, -k), (0.0012, -k + 0.004), (0.004, -0.018), (0.0085, -0.006), (0.009, 0.0),
               (0.0085, 0.006), (0.0055, 0.014), (0.0022, 0.02), (0.0012, 0.022)]
    # Vertex alpha 0.5 marks fluorescent paint (glows a little in M_Tackle), so the float reads from the bank.
    lathe(bm, col, profile, 20, lambda z: (0.85, 0.07, 0.04) if z > 0.004 else (0.92, 0.92, 0.88) if z > -0.012 else (0.10, 0.25, 0.10),
          alpha_fn=lambda z: 0.5 if z > 0.004 else 1.0)
    # Antenna, fluorescent orange tip.
    lathe(bm, col, [(0.0015, 0.022), (0.0014, 0.05), (0.0013, 0.072), (0.0, 0.075)], 10,
          lambda z: (1.0, 0.35, 0.02) if z > 0.045 else (0.95, 0.95, 0.9), alpha_fn=lambda z: 0.5 if z > 0.045 else 1.0)
    # Line eye at the keel.
    tube(bm, col, [Vector((0, 0, -k)) + Vector((0.0018 * math.cos(a), 0, -0.0018 - 0.0018 * math.sin(a)))
                   for a in [2 * math.pi * i / 10 for i in range(11)]], 0.0003, (0.6, 0.6, 0.6), 6, alpha=0.0)
    return bm


def hook_curve():
    """J hook (size ~8): eye at origin, shank down, bend, point back up towards +X."""
    pts = []
    for i in range(9):                                   # eye loop
        a = 2 * math.pi * i / 8
        pts.append(Vector((0.0016 * math.sin(a), 0, -0.0016 + 0.0016 * math.cos(a))))
    pts += [Vector((0, 0, -0.004 - 0.016 * t)) for t in [0.0, 0.33, 0.66, 1.0]]   # shank
    for i in range(1, 13):                               # bend
        a = math.pi * i / 12
        pts.append(Vector((0.0045 - 0.0045 * math.cos(a), 0, -0.020 - 0.0045 * math.sin(a))))
    pts += [Vector((0.009, 0, -0.020 + 0.0075 * t)) for t in [0.3, 0.6, 1.0]]     # point
    return pts


def hook():
    bm, col = new_bm()
    pts = hook_curve()
    tube(bm, col, pts, 0.00045, (0.35, 0.36, 0.38), 6, alpha=0.0, radius_fn=lambda t: 1.0 if t < 0.93 else max(0.15, (1 - t) / 0.07))
    # Barb.
    tube(bm, col, [Vector((0.009, 0, -0.0145)), Vector((0.0075, 0, -0.016))], 0.0003, (0.35, 0.36, 0.38), 5, alpha=0.0)
    return bm


def smooth_path(points, steps=4):
    """Catmull-Rom through the control points so bent baits look organic, not jointed."""
    out = []
    pts = [points[0]] + points + [points[-1]]
    for i in range(1, len(pts) - 2):
        p0, p1, p2, p3 = pts[i - 1], pts[i], pts[i + 1], pts[i + 2]
        for k in range(steps):
            t = k / steps
            out.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t))
    out.append(points[-1])
    return out


def worm():
    bm, col = new_bm()
    # Threaded along the shank and bend, ends hanging freely.
    pts = [Vector((-0.004, 0.002, -0.004)), Vector((-0.002, 0.0, -0.002)), Vector((0.0, -0.001, -0.006)),
           Vector((0.0008, 0.0, -0.012)), Vector((0.0, 0.001, -0.018)), Vector((0.0035, 0.0, -0.0245)),
           Vector((0.008, 0.0, -0.021)), Vector((0.010, -0.002, -0.026)), Vector((0.008, -0.004, -0.034)),
           Vector((0.002, -0.004, -0.040)), Vector((-0.004, -0.002, -0.046)), Vector((-0.006, 0.001, -0.054))]
    tube(bm, col, smooth_path(pts), 0.0028, (0.72, 0.28, 0.30), 10,
         radius_fn=lambda t: 0.55 + 0.45 * math.sin(math.pi * min(1, t * 1.15)),
         colour_fn=lambda t: (0.78, 0.32, 0.34) if int(t * 60) % 3 else (0.62, 0.22, 0.25))
    return bm


def maggot():
    bm, col = new_bm()
    for i, (x, z, ang) in enumerate([(0.0045, -0.026, 0.3), (0.006, -0.022, -0.6), (0.002, -0.022, 1.2)]):
        c = Vector((x, 0.0, z))
        d = Vector((math.cos(ang), math.sin(ang) * 0.4, math.sin(ang) * 0.6)).normalized()
        pts = [c + d * (0.0065 * (t - 0.5)) for t in [k / 6 for k in range(7)]]
        tube(bm, col, pts, 0.0018, (0.92, 0.86, 0.68), 8,
             radius_fn=lambda t: 0.45 + 0.55 * math.sin(math.pi * t) ** 0.6,
             colour_fn=lambda t: (0.94, 0.88, 0.70) if int(t * 7) % 2 else (0.86, 0.80, 0.62))
    return bm


def bread():
    bm, col = new_bm()
    blob(bm, col, Vector((0.0045, 0, -0.022)), (0.0075, 0.0065, 0.007), (0.93, 0.90, 0.80), seed=1.3, lumpiness=0.22)
    return bm


def sinker():
    bm, col = new_bm()
    blob(bm, col, Vector((0, 0, 0)), (0.0022, 0.0022, 0.0022), (0.32, 0.33, 0.34), seed=0.4, segments=10, rings=6, lumpiness=0.05)
    return bm


def reel():
    bm, col = new_bm()
    dark, metal, spool_line = (0.04, 0.04, 0.045), (0.55, 0.56, 0.58), (0.62, 0.68, 0.62)
    # Foot and stem down from the rod.
    tube(bm, col, [Vector((-0.02, 0, 0)), Vector((0.02, 0, 0))], 0.003, dark, 6)
    tube(bm, col, [Vector((0, 0, 0)), Vector((0, 0, -0.035))], 0.004, dark, 8)
    # Body and spool on a forward axis (+X) below the stem.
    body = lathe(bm, col, [(0.001, -0.02), (0.013, -0.018), (0.017, 0.0), (0.016, 0.025), (0.001, 0.028)], 18,
                 lambda z: dark)
    spool = lathe(bm, col, [(0.012, 0.03), (0.02, 0.031), (0.0195, 0.05), (0.017, 0.052), (0.004, 0.056)], 18,
                  lambda z: spool_line if 0.032 < z < 0.049 else metal)
    for ring, _ in body + spool:
        for v in ring:   # turn the Z-lathe so its axis points forward (+X) and sits below the stem
            x, y, z = v.co
            v.co = Vector((z, y, x - 0.05))
    # Handle and knob on the left side.
    tube(bm, col, [Vector((0.0, 0.022, -0.05)), Vector((0.0, 0.04, -0.05)), Vector((0.0, 0.045, -0.025))], 0.0018, metal, 6, alpha=0.0)
    tube(bm, col, [Vector((0.0, 0.045, -0.025)), Vector((0.0, 0.058, -0.025))], 0.004, dark, 8)
    return bm


def handle():
    bm, col = new_bm()
    cork = (0.62, 0.45, 0.28)
    # Butt cap, cork grip, reel seat (around X = 0.32), fore grip.
    pts = [Vector((x, 0, 0)) for x in (0.0, 0.004, 0.03, 0.29, 0.3, 0.36, 0.37, 0.42, 0.44)]
    radii = [0.013, 0.014, 0.0135, 0.0125, 0.011, 0.011, 0.012, 0.0105, 0.007]
    colours = [(0.05, 0.05, 0.05), (0.05, 0.05, 0.05), cork, cork, (0.08, 0.08, 0.09), (0.08, 0.08, 0.09), cork, cork, (0.05, 0.05, 0.05)]
    rings = []
    for p, r in zip(pts, radii):
        rings.append([bm.verts.new(p + Vector((0, math.cos(a) * r, math.sin(a) * r))) for a in [2 * math.pi * k / 16 for k in range(16)]])
    for i in range(len(rings) - 1):
        for k in range(16):
            face(bm, col, [rings[i][k], rings[i + 1][k], rings[i + 1][(k + 1) % 16], rings[i][(k + 1) % 16]], colours[i + 1])
    face(bm, col, rings[0], colours[0])
    return bm


def stand():
    bm, col = new_bm()
    bark, wood = (0.22, 0.15, 0.09), (0.55, 0.42, 0.26)
    stem = [Vector((0.002 * math.sin(t * 7), 0, 0.62 * t - 0.12)) for t in [i / 8 for i in range(9)]]
    tube(bm, col, stem, 0.012, bark, 8, radius_fn=lambda t: 1.0 - 0.25 * t, colour_fn=lambda t: wood if t < 0.03 else bark)
    for side in (1, -1):
        fork = [Vector((0, 0, 0.5)) + Vector((side * 0.06 * t ** 0.8, 0, 0.14 * t)) for t in [i / 5 for i in range(6)]]
        tube(bm, col, fork, 0.008, bark, 7, radius_fn=lambda t: 1.0 - 0.4 * t, colour_fn=lambda t: wood if t > 0.95 else bark)
    return bm


PARTS = {'Bobber': bobber, 'Hook': hook, 'Worm': worm, 'Maggot': maggot, 'Bread': bread,
         'Sinker': sinker, 'Reel': reel, 'RodHandle': handle, 'RodStand': stand}


def main():
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    for name, build in PARTS.items():
        for o in list(bpy.context.scene.objects):
            bpy.data.objects.remove(o, do_unlink=True)
        bm = build()
        mesh = bpy.data.meshes.new('SM_' + name)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        bm.to_mesh(mesh)
        bm.free()
        for p in mesh.polygons:
            p.use_smooth = True
        mesh.materials.append(bpy.data.materials.get('M_Tackle') or bpy.data.materials.new('M_Tackle'))
        obj = bpy.data.objects.new('SM_' + name, mesh)
        bpy.context.scene.collection.objects.link(obj)
        bpy.ops.object.select_all(action='DESELECT')
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        path = os.path.join(OUT, 'SM_%s.fbx' % name)
        bpy.ops.export_scene.fbx(filepath=path, use_selection=True, apply_unit_scale=True, object_types={'MESH'},
                                 mesh_smooth_type='FACE', colors_type='LINEAR', axis_forward='-Y', axis_up='Z',
                                 bake_space_transform=True)
        print('TACKLE_EXPORTED', name, len(mesh.vertices), path)


main()
