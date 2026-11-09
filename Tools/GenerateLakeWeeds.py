"""Underwater plants for the lake bed as FBX files (run with Blender 5.x).

    blender --background --factory-startup --python Tools/GenerateLakeWeeds.py

Real size in metres, origin at the bed. Colours are vertex colours, the vertex alpha is the height
fraction (a little sway of the water current uses it). Every blade is a two-sided strip, normals up.
  Weed_Ribbon   - ribbon weed (Vallisneria): a tuft of long tape leaves, 0.6-1.3 m, gently twisted.
  Weed_Elodea   - elodea (waterweed): short stems with whorls of small leaves, 0.35-0.65 m.
  Weed_Pondweed - pondweed (Potamogeton): stems with alternate oval leaves, 0.6-1.2 m.
  Weed_Grass    - submerged lake grass: a dense tuft of thin short blades, 0.3-0.6 m.
  Weed_Hornwort - hornwort (Ceratophyllum): dark feathery whorls of needles, 0.5-0.9 m.
  Weed_Chara    - chara algae: a low dense cushion of tiny strands, 0.1-0.25 m.
Output: Tools/GeneratedLakeWeeds/SM_<Name>.fbx
"""
import math
import os
import random

import bmesh
import bpy
from mathutils import Vector

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'GeneratedLakeWeeds')


def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def jitter(colour, rnd, amount):
    k = rnd.uniform(-amount, amount)
    return tuple(max(0.0, c + k) for c in colour)


def horizontal(yaw):
    return Vector((math.cos(yaw), math.sin(yaw), 0.0))


class Builder:
    """A bmesh with a vertex-colour layer; alpha is the height of a vertex over the plant's top."""

    def __init__(self, top):
        self.bm = bmesh.new()
        self.col = self.bm.loops.layers.color.new('Col')
        self.top = top

    def strip(self, points, widths, sides, colours):
        prev = None
        for p, w, s, c in zip(points, widths, sides, colours):
            row = (self.bm.verts.new(p - s * w), self.bm.verts.new(p + s * w), p, c)
            if prev:
                f = self.bm.faces.new([prev[0], prev[1], row[1], row[0]])
                for loop, (pt, cc) in zip(f.loops, ((prev[2], prev[3]), (prev[2], prev[3]), (row[2], row[3]), (row[2], row[3]))):
                    loop[self.col] = (*cc, min(1.0, max(0.0, pt.z / self.top)))
            prev = row


def leaf(b, rnd, start, yaw, length, width, rise, droop, base_colour, tip_colour, segments=6, twist=0.0):
    points, widths, sides, colours = [], [], [], []
    p = start.copy()
    step = length / segments
    for i in range(segments + 1):
        t = i / segments
        angle = math.radians(rise + droop * t ** 1.6)
        d = horizontal(yaw) * math.sin(angle) + Vector((0, 0, math.cos(angle)))
        points.append(p.copy())
        widths.append(width * (1.0 - t ** 3) * (0.35 + 0.65 * min(1.0, t * 6.0)) + 0.0004)
        sides.append(horizontal(yaw + math.pi / 2 + twist * t))
        colours.append(mix(base_colour, tip_colour, t))
        p = p + d * step
    b.strip(points, widths, sides, colours)
    return p


def tuft(seed, blades, radius, h_min, h_max, width, rise, droop, base_colour, tip_colour, twist=0.0):
    rnd = random.Random(seed)
    b = Builder(h_max)
    lean = rnd.uniform(0, 2 * math.pi)
    for _ in range(blades):
        a, r = rnd.uniform(0, 2 * math.pi), radius * math.sqrt(rnd.random())
        base = Vector((math.cos(a) * r, math.sin(a) * r, -0.02))
        leaf(b, rnd, base, lean + rnd.uniform(-0.9, 0.9), rnd.uniform(h_min, h_max), width * rnd.uniform(0.7, 1.3),
             rise + rnd.uniform(-12, 12), droop + rnd.uniform(-20, 30), jitter(base_colour, rnd, 0.015),
             jitter(tip_colour, rnd, 0.02), twist=twist)
    return b


def vallisneria(seed):
    return tuft(seed, 16, 0.16, 0.65, 1.3, 0.022, 10, 55, (0.045, 0.115, 0.04), (0.10, 0.22, 0.085), twist=0.55)


def lake_grass(seed):
    return tuft(seed, 40, 0.2, 0.3, 0.62, 0.011, 12, 45, (0.04, 0.10, 0.04), (0.09, 0.20, 0.075), twist=0.2)


def elodea(seed):
    rnd = random.Random(seed)
    b = Builder(0.65)
    for _ in range(6):
        base = Vector((rnd.uniform(-0.12, 0.12), rnd.uniform(-0.12, 0.12), -0.02))
        height = rnd.uniform(0.35, 0.62)
        yaw = rnd.uniform(0, 2 * math.pi)
        p = base
        for step in range(5):
            t = step / 5
            nxt = p + Vector((0, 0, height / 5)) + horizontal(yaw) * 0.02
            for k in range(4):
                leaf(b, rnd, p, yaw + k * math.pi / 2 + 0.4, rnd.uniform(0.05, 0.09), 0.012, 35, 45,
                     jitter((0.06, 0.14, 0.055), rnd, 0.01), jitter((0.12, 0.25, 0.10), rnd, 0.015), segments=3)
            p = nxt
    return b


def pondweed(seed):
    rnd = random.Random(seed)
    b = Builder(1.2)
    for _ in range(7):
        base = Vector((rnd.uniform(-0.16, 0.16), rnd.uniform(-0.16, 0.16), -0.02))
        height = rnd.uniform(0.6, 1.2)
        yaw = rnd.uniform(0, 2 * math.pi)
        p = base
        leaf(b, rnd, p, yaw, height, 0.008, 4, 22, (0.05, 0.10, 0.04), (0.09, 0.17, 0.06), segments=7)
        for step in range(6):
            t = (step + 1) / 7
            p = base + horizontal(yaw) * (0.1 * t * t * height) + Vector((0, 0, height * t))
            for k in (-1, 1):
                leaf(b, rnd, p, yaw + k * math.pi / 2 + rnd.uniform(-0.3, 0.3), rnd.uniform(0.05, 0.09), 0.03, 60, 25,
                     jitter((0.07, 0.145, 0.055), rnd, 0.01), jitter((0.13, 0.23, 0.08), rnd, 0.015), segments=4)
    return b


def hornwort(seed):
    rnd = random.Random(seed)
    b = Builder(0.9)
    for _ in range(5):
        base = Vector((rnd.uniform(-0.14, 0.14), rnd.uniform(-0.14, 0.14), -0.02))
        height = rnd.uniform(0.5, 0.9)
        yaw = rnd.uniform(0, 2 * math.pi)
        p = base
        for step in range(9):
            t = step / 9
            p = base + horizontal(yaw) * (0.08 * t * t * height) + Vector((0, 0, height * t))
            for k in range(5):
                leaf(b, rnd, p, rnd.uniform(0, 2 * math.pi), rnd.uniform(0.04, 0.07), 0.006, 70, 25,
                     jitter((0.035, 0.08, 0.035), rnd, 0.008), jitter((0.07, 0.145, 0.06), rnd, 0.012), segments=3)
    return b


def chara(seed):
    return tuft(seed, 70, 0.34, 0.10, 0.25, 0.007, 20, 30, (0.075, 0.11, 0.045), (0.13, 0.17, 0.06), twist=0.3)


PARTS = {
    'Weed_Ribbon': vallisneria,
    'Weed_Grass': lake_grass,
    'Weed_Elodea': elodea,
    'Weed_Pondweed': pondweed,
    'Weed_Hornwort': hornwort,
    'Weed_Chara': chara,
}


def main():
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    for index, (name, build) in enumerate(PARTS.items()):
        for o in list(bpy.context.scene.objects):
            bpy.data.objects.remove(o, do_unlink=True)
        builder = build(1000 + index * 17)
        bm = builder.bm
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0004)
        mesh = bpy.data.meshes.new('SM_' + name)
        bm.to_mesh(mesh)
        bm.free()
        obj = bpy.data.objects.new('SM_' + name, mesh)
        bpy.context.scene.collection.objects.link(obj)
        bpy.ops.object.select_all(action='DESELECT')
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        path = os.path.join(OUT, 'SM_%s.fbx' % name)
        bpy.ops.export_scene.fbx(filepath=path, use_selection=True, apply_unit_scale=True, object_types={'MESH'},
                                 mesh_smooth_type='FACE', colors_type='LINEAR', axis_forward='-Y', axis_up='Z',
                                 bake_space_transform=True)
        print('LAKE_WEED_EXPORTED', name, len(mesh.vertices), path)


main()
