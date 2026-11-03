"""Generate the village props as FBX files (run with Blender 5.x).

    blender --background --factory-startup --python Tools/GenerateVillageModels.py

Real size in metres. Each part uses up to five material slots that Unreal maps to its materials
(ImportVillage.py): Wood = weathered boards and logs, Bark = poles and stakes, Paint = vertex colour
paint and tar, Glass = dark window panes, Net = a masked netting grid (needs UVs).
  Boat_A / Boat_B - a 4.2 m wooden rowboat with three thwarts and a pair of oars; origin at the
                    keel's midpoint, bow towards +X. A: faded green strake, B: faded red-brown.
  Izba            - a log house 6.4 x 7.2 m with a plank roof, porch and carved white window frames;
                    origin at the ground in the middle, the front with the porch facing +X.
  NetRack         - two forked posts with poles and a fishing net hung to dry; origin at the ground.
  Fence           - a 3 m section of a rail fence; origin at the ground at its start, along +X.
Output: Tools/GeneratedVillage/SM_<Part>.fbx
"""
import math
import os
import random
import bmesh
import bpy
from mathutils import Vector, Matrix

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'GeneratedVillage')
SLOTS = ['Wood', 'Bark', 'Paint', 'Glass', 'Net']
WOOD, BARK, PAINT, GLASS, NET = range(5)


class Builder:
    def __init__(self):
        self.bm = bmesh.new()
        self.col = self.bm.loops.layers.color.new('Col')
        self.uv = self.bm.loops.layers.uv.new('UVMap')

    def face(self, verts, slot, colour=(0.5, 0.5, 0.5), uvs=None):
        try:
            f = self.bm.faces.new(verts)
        except ValueError:
            return None
        f.material_index = slot
        for i, loop in enumerate(f.loops):
            loop[self.col] = (*colour, 1.0)
            loop[self.uv].uv = uvs[i] if uvs else (loop.vert.co.x + loop.vert.co.z, loop.vert.co.y + loop.vert.co.z)
        return f

    def box(self, centre, size, slot, colour=(0.5, 0.5, 0.5), rot=None):
        """Box of `size` (x, y, z) around `centre`, optionally rotated by a 3x3 matrix."""
        rot = rot or Matrix.Identity(3)
        hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
        corners = [Vector((sx * hx, sy * hy, sz * hz)) for sz in (-1, 1) for sy in (-1, 1) for sx in (-1, 1)]
        v = [self.bm.verts.new(centre + rot @ c) for c in corners]
        for q in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
            self.face([v[i] for i in q], slot, colour)

    def cylinder(self, a, b, radius, slot, colour=(0.5, 0.5, 0.5), segments=10, wobble=0.0, seed=0):
        """Cylinder from point a to point b, slightly irregular for logs and poles."""
        rnd = random.Random(seed)
        axis = (b - a)
        d = axis.normalized()
        side = d.cross(Vector((0, 0, 1)))
        if side.length < 1e-3:
            side = d.cross(Vector((1, 0, 0)))
        side.normalize()
        up = side.cross(d)
        rings = []
        steps = max(1, int(axis.length / 0.6))
        for i in range(steps + 1):
            t = i / steps
            c = a + axis * t
            r = radius * (1 + wobble * rnd.uniform(-1, 1))
            rings.append([self.bm.verts.new(c + (side * math.cos(q) + up * math.sin(q)) * r)
                          for q in [2 * math.pi * k / segments for k in range(segments)]])
        for r0, r1 in zip(rings, rings[1:]):
            for k in range(segments):
                self.face([r0[k], r0[(k + 1) % segments], r1[(k + 1) % segments], r1[k]], slot, colour)
        self.face(rings[0][::-1], slot, colour)
        self.face(rings[-1], slot, colour)


def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


# --- boat ------------------------------------------------------------------------------------------

def boat(stripe, seed):
    B = Builder()
    L, beam, depth = 4.2, 1.18, 0.46
    tar, inside = (0.035, 0.03, 0.025), (0.16, 0.13, 0.095)
    stations = 28

    def half_beam(t):          # at the gunwale: a transom at the stern (t = 0), pointed bow (t = 1)
        return max(0.012, beam / 2 * math.sin(math.pi * (0.08 + 0.92 * t)) ** 0.55)

    def section(t):
        wg = half_beam(t)
        wb = wg * 0.62
        zb = 0.05 + 0.25 * (2 * t - 1) ** 4                                         # rocker
        zs = depth + 0.18 * max(0.0, 2 * t - 1) ** 2 + 0.06 * max(0.0, 1 - 2 * t) ** 2   # sheer
        star = [(wb, zb), (wg * 0.93, zb + (zs - zb) * 0.45), (wg, zs)]
        return [(-y, z) for (y, z) in star[::-1]] + [(0.0, zb)] + star

    outer, inner = [], []
    for k in range(stations + 1):
        t = k / stations
        x = t * L - L / 2
        o_row, i_row = [], []
        for (y, z) in section(t):
            o_row.append(B.bm.verts.new(Vector((x, y, z))))
            inset = 0.025 if abs(y) > 0.03 else 0.0
            i_row.append(B.bm.verts.new(Vector((x, y - math.copysign(inset, y) if y else 0.0, z + 0.025))))
        outer.append(o_row)
        inner.append(i_row)
    n = len(outer[0])
    for k in range(stations):
        for j in range(n - 1):
            upper = j in (0, n - 2)                     # the top strake is painted, the rest tarred
            c = stripe if upper else mix(tar, (0.06, 0.05, 0.04), (j % 2) * 0.5)
            B.face([outer[k][j], outer[k][j + 1], outer[k + 1][j + 1], outer[k + 1][j]], PAINT, c)
            B.face([inner[k][j], inner[k + 1][j], inner[k + 1][j + 1], inner[k][j + 1]], PAINT, mix(inside, (0.19, 0.16, 0.12), (j % 2) * 0.6))
        for j in (0, n - 1):                            # gunwale rim
            B.face([outer[k][j], outer[k + 1][j], inner[k + 1][j], inner[k][j]], WOOD)
    for j in range(n - 1):                              # transom
        B.face([outer[0][j], inner[0][j], inner[0][j + 1], outer[0][j + 1]], PAINT, tar)
    # Thwarts sized to the hull, a floorboard and the oars.
    for t, h in ((0.22, 0.34), (0.5, 0.36), (0.74, 0.39)):
        B.box(Vector((t * L - L / 2, 0, h)), (0.22, half_beam(t) * 1.9, 0.035), WOOD)
    B.box(Vector((-0.1, 0, 0.09)), (2.4, 0.34, 0.02), WOOD)
    rnd = random.Random(seed)
    for side in (-1, 1):
        a = Vector((-1.25, side * 0.3, 0.4))
        b = a + Vector((2.3, side * 0.05 * rnd.uniform(0.5, 1), 0.03))
        B.cylinder(a, b, 0.022, WOOD, segments=6)
        B.box(b + Vector((0.18, 0, 0)), (0.42, 0.13, 0.015), WOOD)
    return B


# --- log house -------------------------------------------------------------------------------------

def izba():
    B = Builder()
    rnd = random.Random(7)
    W, D, R = 7.2, 6.4, 0.14          # along X (front-back), along Y, log radius
    courses = 12
    over = 0.32                       # log ends stick out at the corners
    silver = (0.5, 0.5, 0.5)
    for c in range(courses):
        z = 0.45 + R + c * 2 * R * 0.92
        shift = R * 0.92 if c % 2 else 0.0
        # Long walls along X at y = +-D/2 and short walls along Y at x = +-W/2, interlocked.
        for y in (-D / 2, D / 2):
            B.cylinder(Vector((-W / 2 - over, y, z + shift)), Vector((W / 2 + over, y, z + shift)), R * rnd.uniform(0.93, 1.05), WOOD, silver, 10, 0.04, c * 3 + 1)
        for x in (-W / 2, W / 2):
            B.cylinder(Vector((x, -D / 2 - over, z + R * 0.92 - shift)), Vector((x, D / 2 + over, z + R * 0.92 - shift)), R * rnd.uniform(0.93, 1.05), WOOD, silver, 10, 0.04, c * 3 + 2)
    top = 0.45 + courses * 2 * R * 0.92 + R
    # Stone footing under the corners.
    for x in (-W / 2, W / 2, 0):
        for y in (-D / 2, D / 2):
            B.box(Vector((x, y, 0.2)), (0.55, 0.55, 0.45), BARK, (0.3, 0.3, 0.3))
    # Gables (vertical boards) and the plank roof, eaves overhanging.
    ridge = top + 2.3
    for x in (-W / 2 - 0.05, W / 2 + 0.05):
        for k in range(32):
            y0 = -D / 2 + D * k / 32
            y1 = y0 + D / 32 * 0.96
            h0 = top + (ridge - top) * (1 - abs(y0 + D / 64) / (D / 2))
            B.box(Vector((x, (y0 + y1) / 2, (top + h0) / 2)), (0.04, y1 - y0, max(0.05, h0 - top)), WOOD)
    slope = math.atan2(ridge - top, D / 2 + 0.6)
    length = math.hypot(ridge - top, D / 2 + 0.6) + 0.1
    for side in (-1, 1):
        rot = Matrix.Rotation(-side * slope, 3, 'X')
        for k in range(26):
            x = -W / 2 - 0.7 + (W + 1.4) * (k + 0.5) / 26
            centre = Vector((x, side * (D / 2 + 0.6) / 2, (top + ridge) / 2 - 0.02))
            B.box(centre, ((W + 1.4) / 26 * 0.97, length, 0.035), WOOD, rot=rot)
    B.box(Vector((0, 0, ridge + 0.05)), (W + 1.5, 0.22, 0.12), WOOD)
    # Front (+X): door, porch with steps; windows with white carved frames on the front and sides.
    fx = W / 2 + R
    B.box(Vector((fx + 0.02, 0.9, 1.45)), (0.06, 1.0, 1.9), PAINT, (0.12, 0.1, 0.08))
    B.box(Vector((fx + 0.75, 0.9, 0.45)), (1.4, 1.6, 0.06), WOOD)
    for k in range(3):
        B.box(Vector((fx + 1.55 + k * 0.28, 0.9, 0.12 + k * 0.11)), (0.3, 1.2, 0.05), WOOD)
    for px in (fx + 1.35,):
        for py in (0.15, 1.65):
            B.cylinder(Vector((px, py, 0.45)), Vector((px, py, 2.6)), 0.06, BARK)
    B.box(Vector((fx + 0.8, 0.9, 2.65)), (1.6, 1.8, 0.05), WOOD)

    def window(centre, normal):
        side = Vector((-normal.y, normal.x, 0))
        B.box(centre + normal * 0.01, (0.04, 0.04, 0.04), GLASS)          # anchor so the slot exists
        pane = [centre + normal * 0.03 + side * sx * 0.38 + Vector((0, 0, sz * 0.5)) for sx, sz in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        v = [B.bm.verts.new(p) for p in pane]
        B.face(v if normal.x + normal.y > 0 else v[::-1], GLASS)
        white, blue = (0.85, 0.85, 0.82), (0.22, 0.35, 0.55)
        for sz in (-0.58, 0.58):
            B.box(centre + normal * 0.06 + Vector((0, 0, sz)), (0.1 if normal.x else 1.0, 1.0 if normal.x else 0.1, 0.1), PAINT, white)
        for sx in (-0.46, 0.46):
            B.box(centre + normal * 0.06 + side * sx, (0.08 if normal.x else 0.12, 0.12 if normal.x else 0.08, 1.25), PAINT, white)
        # Carved crest above the window and blue shutters.
        B.box(centre + normal * 0.07 + Vector((0, 0, 0.78)), (0.1 if normal.x else 1.2, 1.2 if normal.x else 0.1, 0.22), PAINT, white)
        for sx in (-0.75, 0.75):
            B.box(centre + normal * 0.08 + side * sx, (0.04 if normal.x else 0.4, 0.4 if normal.x else 0.04, 1.05), PAINT, blue)

    for y in (-1.6,):
        window(Vector((fx + 0.02, y, 1.9)), Vector((1, 0, 0)))
    for x in (-1.8, 1.4):
        window(Vector((x, D / 2 + R + 0.02, 1.9)), Vector((0, 1, 0)))
        window(Vector((x, -D / 2 - R - 0.02, 1.9)), Vector((0, -1, 0)))
    return B


# --- net rack and fence -----------------------------------------------------------------------------

def net_rack():
    B = Builder()
    rnd = random.Random(3)
    span = 3.4
    for x in (-span / 2, span / 2):
        B.cylinder(Vector((x, 0, -0.3)), Vector((x + rnd.uniform(-0.05, 0.05), 0, 1.9)), 0.055, BARK, seed=int(x * 10))
        for s in (-1, 1):   # fork
            B.cylinder(Vector((x, 0, 1.75)), Vector((x + s * 0.12, 0, 2.05)), 0.03, BARK)
    B.cylinder(Vector((-span / 2 - 0.3, 0, 1.92)), Vector((span / 2 + 0.3, 0, 1.92)), 0.04, BARK, segments=8)
    # The net hangs from the pole in a sagging sheet, a little to each side.
    cols, rows = 24, 10
    grid = []
    for i in range(rows + 1):
        t = i / rows
        row = []
        for j in range(cols + 1):
            u = j / cols
            x = -span / 2 + span * u
            sag = 0.25 * math.sin(math.pi * u) * t
            row.append(B.bm.verts.new(Vector((x, 0.05 * math.sin(u * 9 + t * 4) + 0.12 * t * math.sin(u * 3), 1.9 - 1.5 * t - sag))))
        grid.append(row)
    for i in range(rows):
        for j in range(cols):
            uv = [(j * 4.0, i * 4.0), (j * 4.0 + 4, i * 4.0), (j * 4.0 + 4, i * 4.0 + 4), (j * 4.0, i * 4.0 + 4)]
            B.face([grid[i][j], grid[i][j + 1], grid[i + 1][j + 1], grid[i + 1][j]], NET, (0.45, 0.43, 0.38),
                   [uv[0], uv[1], uv[2], uv[3]])
    # Cork floats along the top edge.
    for j in range(0, cols + 1, 3):
        p = grid[0][j].co
        B.box(p + Vector((0, 0, -0.04)), (0.06, 0.04, 0.05), PAINT, (0.55, 0.4, 0.2))
    return B


def fence():
    B = Builder()
    rnd = random.Random(5)
    for x in (0.0, 3.0):
        B.cylinder(Vector((x, 0, -0.3)), Vector((x + rnd.uniform(-0.04, 0.04), rnd.uniform(-0.04, 0.04), 1.3)), 0.06, BARK, seed=int(x))
    for z in (0.45, 0.85, 1.2):
        B.cylinder(Vector((-0.15, 0.07, z + rnd.uniform(-0.03, 0.03))), Vector((3.15, 0.07, z + rnd.uniform(-0.05, 0.05))), 0.035, BARK, segments=7, wobble=0.1, seed=int(z * 10))
    return B


PARTS = {
    'Boat_A': lambda: boat((0.17, 0.27, 0.19), 1),
    'Boat_B': lambda: boat((0.36, 0.14, 0.09), 2),
    'Izba': izba,
    'NetRack': net_rack,
    'Fence': fence,
}


def main():
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    materials = [bpy.data.materials.new('M_' + s) for s in SLOTS]
    for name, build in PARTS.items():
        for o in list(bpy.context.scene.objects):
            bpy.data.objects.remove(o, do_unlink=True)
        builder = build()
        bm = builder.bm
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0005)
        mesh = bpy.data.meshes.new('SM_' + name)
        bm.to_mesh(mesh)
        bm.free()
        for m in materials:
            mesh.materials.append(m)
        obj = bpy.data.objects.new('SM_' + name, mesh)
        bpy.context.scene.collection.objects.link(obj)
        bpy.ops.object.select_all(action='DESELECT')
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        path = os.path.join(OUT, 'SM_%s.fbx' % name)
        bpy.ops.export_scene.fbx(filepath=path, use_selection=True, apply_unit_scale=True, object_types={'MESH'},
                                 mesh_smooth_type='FACE', colors_type='LINEAR', axis_forward='-Y', axis_up='Z',
                                 bake_space_transform=True)
        print('VILLAGE_EXPORTED', name, len(mesh.vertices), path)


main()
