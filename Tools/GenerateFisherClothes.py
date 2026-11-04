"""Village angler's clothes for the MetaHuman MH_Fisher, built around its exported body in Blender:
a quilted jacket (vatnik) with a standing collar, a knitted roll-neck sweater showing at the neck,
canvas trousers tucked into rubber boots, and a flat cap. The jacket, sweater and trousers are grown
from the body surface, so they keep its bone weights; the boots and the cap are modelled and take the
weights of the nearest body points. The body itself is cut down to the hands, the only skin the
clothes leave open.

    blender --background --factory-startup --python Tools/GenerateFisherClothes.py -- \
        Saved/FisherBody/MH_Fisher_Body.fbx Saved/FisherBody/MH_Fisher_Head.fbx Saved/FisherBody

Writes Fisher_Outfit.fbx (hands + clothes, one skeletal mesh) and Fisher_Cap.fbx (hidden in the
first person) into the output folder and preview renders Saved/FisherClothes_*.png.
The body faces -Y here (UE +Y after import), x > 0 is the left side, metres.
"""
import bpy, bmesh, sys, math, os
from mathutils import Vector, Matrix, noise, kdtree

argv = sys.argv[sys.argv.index("--") + 1:]
BODY_FBX, HEAD_FBX, OUT_DIR = argv[0], argv[1], argv[2]
PREVIEW = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Saved", "FisherClothes_")

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=BODY_FBX)
arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
for o in list(bpy.data.objects):
    if o.type == 'MESH' and not o.name.endswith("LOD0"):
        bpy.data.objects.remove(o)
body = next(o for o in bpy.data.objects if o.type == 'MESH')
body.name = "Body"


def clear_custom_normals(obj):
    bpy.context.view_layer.objects.active = obj
    if obj.data.has_custom_normals:
        bpy.ops.mesh.customdata_custom_splitnormals_clear()


# The head mesh reaches down over the neck and the top of the chest: weld that part to the body,
# so the sweater and the jacket cover it too. A full copy of the head stays for the previews.
before = set(bpy.data.objects)
bpy.ops.import_scene.fbx(filepath=HEAD_FBX)
head_objs = [o for o in bpy.data.objects if o not in before]
head = next(o for o in head_objs if o.type == 'MESH' and o.name.endswith("LOD0"))
head_arm = next(o for o in head_objs if o.type == 'ARMATURE')
for o in head_objs:
    if o is not head and o.type == 'MESH':
        bpy.data.objects.remove(o)
preview_head = head.copy()
preview_head.data = head.data.copy()
preview_head.name = "HeadPreview"
bpy.context.collection.objects.link(preview_head)
NECK_TOP = 1.605
clear_custom_normals(head)
bm = bmesh.new(); bm.from_mesh(head.data)
HM = head.matrix_world
bmesh.ops.delete(bm, geom=[f for f in bm.faces if any((HM @ v.co).z > NECK_TOP for v in f.verts)], context='FACES')
bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
bm.to_mesh(head.data); bm.free()
# The neck skin of the head mesh hangs on facial helper joints the body skeleton does not have:
# move those weights to the nearest ancestor the body has (neck_01, neck_02, head), or they are
# lost on export and the sweater's neck stays behind when the head turns.
body_bones = set(arm.data.bones.keys())


def body_ancestor(name):
    b = head_arm.data.bones.get(name)
    while b and b.name not in body_bones:
        b = b.parent
    return b.name if b else None


groups = {g.index: g.name for g in head.vertex_groups}
targets = {}
for v in head.data.vertices:
    moved = {}
    for g in v.groups:
        name = groups[g.group]
        if name not in body_bones and g.weight > 0:
            to = body_ancestor(name)
            if to:
                moved[to] = moved.get(to, 0.0) + g.weight
    for to, w in moved.items():
        grp = targets.get(to) or head.vertex_groups.get(to) or head.vertex_groups.new(name=to)
        targets[to] = grp
        grp.add([v.index], w, 'ADD')
for g in list(head.vertex_groups):
    if g.name not in body_bones:
        head.vertex_groups.remove(g)
bpy.ops.object.select_all(action='DESELECT')
head.select_set(True); body.select_set(True)
bpy.context.view_layer.objects.active = body
bpy.ops.object.join()

# The exported meshes are split at UV seams: weld them so offsets do not tear the surface.
clear_custom_normals(body)
bm = bmesh.new(); bm.from_mesh(body.data)
bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.02)
bm.to_mesh(body.data); bm.free()


def bone(name, tail=False):
    b = arm.data.bones[name]
    return arm.matrix_world @ (b.tail_local if tail else b.head_local)


BONES = set(arm.data.bones.keys())
GROUPS = {g.index: g.name for g in body.vertex_groups}


def category(name):
    if 'toe' in name or name.startswith(('foot', 'ball')):
        return 'foot'
    if name.startswith(('hand_', 'thumb_', 'index_', 'middle_', 'ring_', 'pinky_')):
        return 'hand'
    for prefix in ('lowerarm', 'upperarm', 'thigh', 'calf'):
        if name.startswith(prefix):
            return prefix
    return 'torso'


CATEGORY = {i: category(n) for i, n in GROUPS.items()}
M = body.matrix_world
POS = [M @ v.co for v in body.data.vertices]
WEIGHTS = [{GROUPS[g.group]: g.weight for g in v.groups if g.weight > 0 and GROUPS[g.group] in BONES} for v in body.data.vertices]
CAT = []
for w in WEIGHTS:
    c = {}
    for name, x in w.items():
        k = category(name)
        c[k] = c.get(k, 0.0) + x
    CAT.append(max(c, key=c.get) if c else 'torso')


def side_of(p):
    return 'l' if p.x > 0 else 'r'


def arm_param(p, side):
    """0 at the elbow, 1 at the wrist, along the forearm of one side."""
    a, h = bone('lowerarm_' + side), bone('hand_' + side)
    d = h - a
    return (p - a).dot(d) / d.length_squared


CROTCH = min(p.z for p, c in zip(POS, CAT) if c == 'torso' and abs(p.x) < 0.012 and p.z > 0.5)
HEM = CROTCH + 0.035              # the jacket body ends just above the crotch, a skirt hangs below
CHEST = bone('upperarm_l').z - 0.12
BOOT_TOP = bone('calf_l').z - 0.10
NECK = bone('neck_01')
print("FISHER_CLOTHES crotch %.3f hem %.3f chest %.3f boot %.3f" % (CROTCH, HEM, CHEST, BOOT_TOP))


# --- helpers ---------------------------------------------------------------------------------
def garment_from_body(name, keep):
    """A copy of the body with only the faces whose vertices all pass keep(index)."""
    obj = body.copy()
    obj.data = body.data.copy()
    obj.name = obj.data.name = name
    bpy.context.collection.objects.link(obj)
    bm = bmesh.new(); bm.from_mesh(obj.data)
    bmesh.ops.transform(bm, matrix=M, verts=bm.verts)   # edit in world metres
    bm.verts.ensure_lookup_table()
    gone = [f for f in bm.faces if not all(keep(v.index) for v in f.verts)]
    bmesh.ops.delete(bm, geom=gone, context='FACES')
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context='VERTS')
    return obj, bm


def axis_radial(p, a, b):
    d = b - a
    t = max(0.0, min(1.0, (p - a).dot(d) / d.length_squared))
    c = a + d * t
    return p - c, c


def loosen(p, a, b, min_r, extra):
    """Push p away from the bone segment a-b to at least min_r, plus extra: a looser tube."""
    r, c = axis_radial(p, a, b)
    dist = r.length
    if dist < 1e-6:
        return p
    return c + r * ((max(dist, min_r) + extra) / dist)


def border_edges(bm, test=lambda e: True):
    return [e for e in bm.edges if e.is_boundary and test(e)]


def smooth(bm, iterations, factor=0.5):
    verts = [v for v in bm.verts if not v.is_boundary]
    for _ in range(iterations):
        bmesh.ops.smooth_vert(bm, verts=verts, factor=factor, use_axis_x=True, use_axis_y=True, use_axis_z=True)


def smooth_borders(bm, iterations, factor=0.5):
    """Even out the jagged cut along every open edge, so hems and cuffs run straight."""
    border = [v for v in bm.verts if v.is_boundary]
    for _ in range(iterations):
        new = {}
        for v in border:
            ns = [e.other_vert(v) for e in v.link_edges if e.is_boundary]
            if len(ns) == 2:
                new[v] = v.co.lerp((ns[0].co + ns[1].co) * 0.5, factor)
        for v, co in new.items():
            v.co = co
    smooth(bm, 2, 0.3)


def offset_along_normals(bm, amount):
    bm.normal_update()
    for v in bm.verts:
        v.co += v.normal * amount(v)


def extrude_border(bm, edges, move):
    """Extrude open edges once; move(new_vert) places the new ring."""
    ret = bmesh.ops.extrude_edge_only(bm, edges=edges)
    for g in ret['geom']:
        if isinstance(g, bmesh.types.BMVert):
            move(g)
    return [g for g in ret['geom'] if isinstance(g, bmesh.types.BMEdge) and g.is_boundary]


def material(name, colour):
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    mat.diffuse_color = colour
    return mat


def adopt(obj):
    """Parent a modelled object to the armature like the body (armature space, centimetres)."""
    obj.parent = arm
    obj.matrix_parent_inverse = Matrix.Identity(4)
    obj.matrix_basis = Matrix.Identity(4)
    bpy.context.view_layer.update()


def finish(obj, bm, mat, thickness):
    """Back from world metres to the object's space, material, smooth shading, cloth thickness."""
    bmesh.ops.dissolve_degenerate(bm, edges=bm.edges, dist=1e-5)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bmesh.ops.transform(bm, matrix=obj.matrix_world.inverted(), verts=bm.verts)
    bm.normal_update()
    bm.to_mesh(obj.data); bm.free()
    me = obj.data
    me.materials.clear()
    me.materials.append(mat)
    for p in me.polygons:
        p.use_smooth = True
    if thickness > 0:
        mod = obj.modifiers.new("Thickness", 'SOLIDIFY')
        mod.thickness = thickness / obj.matrix_world.to_scale().x
        mod.offset = -1.0
        mod.use_rim = True
        mod.use_even_offset = False
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.modifier_move_to_index(modifier="Thickness", index=0)
        bpy.ops.object.modifier_apply(modifier="Thickness")
    return obj


SLEEVE_X = 0.21


def in_sleeve(p):
    return abs(p.x) > SLEEVE_X and p.z > CHEST - 0.15


def garment_uv(obj, mode):
    """UV0 in metres with a consistent direction, for tiled fabric and the quilting channels:
    around the torso and the legs (U) and up (V); along the sleeves (U) and around them (V)."""
    me = obj.data
    uv = me.uv_layers.new(name="GarmentUV")
    while len(me.uv_layers) > 1:
        me.uv_layers.remove(me.uv_layers[0])
    R = 14 * 0.08 / (2 * math.pi)   # 14 channels of 8 cm round the body: no seam jump at the back
    for loop in me.loops:
        p = obj.matrix_world @ me.vertices[loop.vertex_index].co
        if mode == 'jacket' and in_sleeve(p):
            s = side_of(p)
            a = bone('upperarm_' + s); h = bone('hand_' + s)
            d = (h - a).normalized()
            r, _ = axis_radial(p, a, h)
            side = Vector((0, 0, 1)).cross(d).normalized()
            ang = math.atan2(r.dot(side), r.dot(Vector((0, 0, 1))))
            uv.data[loop.index].uv = ((p - a).dot(d), ang * 0.07)
        elif mode == 'leg':
            s = side_of(p)
            c = bone('thigh_' + s)
            ang = math.atan2(p.x - c.x, -(p.y - c.y))
            uv.data[loop.index].uv = (ang * 0.09, p.z)
        else:
            ang = math.atan2(p.x, -p.y)
            uv.data[loop.index].uv = (ang * R, p.z)


def wear_colour(obj, wear):
    """Vertex colour R = wear and dirt (0 clean .. 1 worn), read by the clothes material."""
    me = obj.data
    attr = me.color_attributes.new("Col", 'BYTE_COLOR', 'POINT')
    for i, v in enumerate(me.vertices):
        w = max(0.0, min(1.0, wear(obj.matrix_world @ v.co)))
        attr.data[i].color = (w, w, w, 1.0)
    me.color_attributes.active_color = attr


def skin_from_body(obj, allowed, merge=None):
    """Bone weights for a modelled garment: inverse-distance blend of the nearest body points
    whose category passes allowed(index)."""
    ids = [i for i in range(len(POS)) if allowed(i)]
    tree = kdtree.KDTree(len(ids))
    for k, i in enumerate(ids):
        tree.insert(POS[i], k)
    tree.balance()
    groups = {}
    for v in obj.data.vertices:
        p = obj.matrix_world @ v.co
        acc = {}
        for co, k, dist in tree.find_n(p, 8):
            w = 1.0 / max(dist, 0.004) ** 2
            for name, x in WEIGHTS[ids[k]].items():
                if merge:
                    name = merge(name)
                acc[name] = acc.get(name, 0.0) + w * x
        total = sum(acc.values())
        for name, x in acc.items():
            if x / total < 0.01:
                continue
            g = groups.get(name) or obj.vertex_groups.new(name=name)
            groups[name] = g
            g.add([v.index], x / total, 'REPLACE')
    obj.modifiers.new("Armature", 'ARMATURE').object = arm


# --- jacket (vatnik) ---------------------------------------------------------------------------
# The neck measured just above where it meets the shoulders: centre and radius round it, so the
# jacket can stop where the body leaves the neck - at its base all round, front and back.
_slice = [p for p, c in zip(POS, CAT) if c == 'torso' and 1.53 < p.z < 1.60]
NECK_C = bone('neck_02').xy if 'neck_02' in arm.data.bones else NECK.xy
_bins = [9.0] * 36
for _p in _slice:   # the neck is the narrowest part between the shoulders and the jaw
    _d = Vector((_p.x - NECK_C.x, _p.y - NECK_C.y))
    _a = int((math.atan2(_d.x, -_d.y) + math.pi) / (2 * math.pi) * 36) % 36
    _bins[_a] = min(_bins[_a], _d.length)
_mean = sum(b for b in _bins if b < 9) / max(1, sum(1 for b in _bins if b < 9))
_bins = [b if b < 9 else _mean for b in _bins]
NECK_R = [sum(_bins[(a + k) % 36] for k in (-1, 0, 1)) / 3 for a in range(36)]
print("FISHER_NECK centre %.3f %.3f radius %.3f..%.3f" % (NECK_C.x, NECK_C.y, min(NECK_R), max(NECK_R)))


def neck_dist(p):
    return Vector((p.x - NECK_C.x, p.y - NECK_C.y)).length


def neck_gap(p):
    """How far p stands out from the neck surface, horizontally."""
    d = Vector((p.x - NECK_C.x, p.y - NECK_C.y))
    a = int((math.atan2(d.x, -d.y) + math.pi) / (2 * math.pi) * 36) % 36
    return d.length - NECK_R[a]


def in_jacket(i):
    p, c = POS[i], CAT[i]
    if c in ('hand', 'foot', 'calf'):
        return False
    if c == 'lowerarm' and arm_param(p, side_of(p)) > 0.86:
        return False
    if p.z < HEM or p.z > 1.56:
        return False
    if p.z > 1.40 and neck_gap(p) < 0.010:   # up to the base of the neck all round, not up it
        return False
    return True


def drape(bm, verts, centre, top, hang, angles=72, step=0.01, taper=0.0):
    """Cloth that hangs: at every angle round the axis centre(z), the radius below 'top' is no
    smaller than the widest point above it (less taper per metre of drop), times hang(angle)."""
    def polar(p):
        c = centre(p.z)
        d = Vector((p.x - c.x, p.y - c.y))
        ang = math.atan2(d.x, -d.y)
        return d, ang, int((ang + math.pi) / (2 * math.pi) * angles) % angles, int(math.floor(p.z / step))
    widest = {}
    for v in verts:
        d, ang, a, zb = polar(v.co)
        widest[(zb, a)] = max(widest.get((zb, a), 0.0), d.length)
    if not widest:
        return
    lo = min(k[0] for k in widest)
    hi = int(math.floor(top / step))
    env = {}
    for a in range(angles):
        run = 0.0
        for zb in range(hi, lo - 1, -1):
            run = max(run - taper * step, widest.get((zb, a), 0.0))
            env[(zb, a)] = run
    # soften the envelope round the axis
    soft = {}
    for (zb, a), r in env.items():
        soft[(zb, a)] = sum(env.get((zb, (a + k) % angles), r) for k in range(-3, 4)) / 7
    for v in verts:
        if v.co.z > top:
            continue
        d, ang, a, zb = polar(v.co)
        r = d.length
        if r < 1e-5:
            continue
        target = r + max(0.0, soft.get((zb, a), r) - r) * hang(ang)
        k = min(1.0, (top - v.co.z) / 0.06)
        new_r = r + (target - r) * k
        c = centre(v.co.z)
        v.co.x = c.x + d.x / r * new_r
        v.co.y = c.y + d.y / r * new_r


jacket, bm = garment_from_body("Jacket", in_jacket)
# small holes along the head-to-body seam would grow into fins with the collar: close them
bmesh.ops.holes_fill(bm, edges=[e for e in bm.edges if e.is_boundary], sides=16)
torso = [v for v in bm.verts if not in_sleeve(v.co)]
# front and back hang straight from the chest and the belly; the sides less, so the arms still fit
drape(bm, torso, lambda z: Vector((0.0, -0.01)), CHEST, lambda ang: 1.0 - 0.85 * abs(math.sin(ang)) ** 0.7)
for v in bm.verts:
    p = v.co
    if p.z < CHEST - 0.15 or abs(p.x) < SLEEVE_X - 0.05:
        continue
    s = side_of(p)
    up, lo, hd = bone('upperarm_' + s), bone('lowerarm_' + s), bone('hand_' + s)
    t = arm_param(p, s)
    if t < 0:
        q = loosen(p, up, lo, 0.056, 0.004)
    else:
        # the sleeve narrows to a snug cuff at the wrist
        q = loosen(p, lo, hd, 0.046 - 0.014 * max(0.0, t), 0.002)
    # blend in over the shoulder, so the sleeve grows out of the body without a step
    w = max(0.0, min(1.0, (abs(p.x) - (SLEEVE_X - 0.05)) / 0.08))
    v.co = p.lerp(q, w * w * (3 - 2 * w))
offset_along_normals(bm, lambda v: 0.013 if in_sleeve(v.co) else 0.018)
smooth(bm, 30, 0.5)
smooth_borders(bm, 12)
# Quilting: soft puffs between stitched channels, vertical on the body, rings on the sleeves.
bm.normal_update()
for v in bm.verts:
    p = v.co
    if in_sleeve(p):
        s = side_of(p)
        a, h = bone('upperarm_' + s), bone('hand_' + s)
        u = (p - a).dot((h - a).normalized())
    else:
        u = math.atan2(p.x, -p.y) * (14 * 0.08 / (2 * math.pi))
    puff = 0.5 + 0.5 * math.cos(2 * math.pi * u / 0.08)
    fade = 1.0 if in_sleeve(p) else max(0.0, min(1.0, (p.z - HEM - 0.03) / 0.08))
    v.co += v.normal * (0.007 * puff + 0.004 * noise.noise(p * 7.0)) * fade
# hem at one height, then a skirt that falls straight and flares a little
band = [v for v in bm.verts if v.co.z < HEM + 0.07 and not v.is_boundary and not in_sleeve(v.co)]
for _ in range(10):
    bmesh.ops.smooth_vert(bm, verts=band, factor=0.5, use_axis_x=True, use_axis_y=True, use_axis_z=True)
hem = border_edges(bm, lambda e: all(x.co.z < HEM + 0.08 and not in_sleeve(x.co) for x in e.verts))
low = min([x.co.z for x in bm.verts if not x.is_boundary and not in_sleeve(x.co)] + [HEM]) - 0.006
for e in hem:   # below every inner point of the band, so no face folds back over the edge
    for x in e.verts:
        x.co.z = low
smooth_borders(bm, 6)


def skirt(drop, flare):
    def move(v):
        r = Vector((v.co.x, v.co.y + 0.01, 0))
        v.co.z -= drop
        if r.length > 1e-6:
            v.co += r.normalized() * flare
    return move


edges = extrude_border(bm, hem, skirt(0.045, 0.004))
edges = extrude_border(bm, edges, skirt(0.045, 0.004))
# the neckline: the longest open loop round the neck; small holes there are closed, not collared
near = [e for e in bm.edges if e.is_boundary and all(x.co.z > 1.38 and neck_dist(x.co) < 0.17 for x in e.verts)]
loops, seen = [], set()
for e in near:
    if e in seen:
        continue
    group, todo = [], [e]
    seen.add(e)
    while todo:
        cur = todo.pop()
        group.append(cur)
        for v in cur.verts:
            for nxt in v.link_edges:
                if nxt in near and nxt not in seen:
                    seen.add(nxt)
                    todo.append(nxt)
    loops.append(group)
loops.sort(key=len, reverse=True)
for small in loops[1:]:
    if len(small) <= 40:
        bmesh.ops.holes_fill(bm, edges=small, sides=40)
neck = loops[0] if loops else []
smooth_borders(bm, 14)
neck = [e for e in neck if e.is_valid and e.is_boundary]


def collar(rise, out):
    def move(v):
        r = Vector((v.co.x - NECK_C.x, v.co.y - NECK_C.y, 0))
        v.co.z += rise
        if r.length > 1e-6:
            v.co += r.normalized() * out
    return move


# a low standing collar round the neck, about 3 cm, with a rolled edge
edges = extrude_border(bm, neck, collar(0.016, 0.002))
edges = extrude_border(bm, edges, collar(0.014, 0.003))
extrude_border(bm, edges, collar(0.003, 0.007))
# cuffs: a short ring so the sleeve ends in a hem rather than a cut
cuffs = border_edges(bm, lambda e: all(abs(x.co.x) > 0.3 for x in e.verts))


def cuff(v):
    s = side_of(v.co)
    d = (bone('hand_' + s) - bone('lowerarm_' + s)).normalized()
    v.co += d * 0.012


extrude_border(bm, cuffs, cuff)
finish(jacket, bm, material("Jacket", (0.16, 0.17, 0.13, 1)), 0.010)
garment_uv(jacket, 'jacket')
wear_colour(jacket, lambda p: 0.15 + 0.6 * max(0.0, 1.0 - (p.z - (HEM - 0.09)) / 0.10)
            + 0.6 * max(0.0, abs(p.x) - 0.40) / 0.08)


# --- sweater: a knitted roll neck showing in the collar -----------------------------------------
def in_sweater(i):
    p = POS[i]
    back = max(0.0, min(1.0, (p.y - (NECK.y - 0.06)) / 0.12))
    near_neck = neck_dist(p) < 0.12        # only a yoke: it shows in the collar
    return CAT[i] == 'torso' and near_neck and 1.38 < p.z < 1.525 + 0.075 * back   # up the neck, under the jaw


sweater, bm = garment_from_body("Sweater", in_sweater)
offset_along_normals(bm, lambda v: 0.010)
smooth(bm, 8, 0.4)
smooth_borders(bm, 6)
top = border_edges(bm, lambda e: all(x.co.z > 1.50 and neck_gap(x.co) < 0.04 for x in e.verts))


def roll(v):
    r = Vector((v.co.x, v.co.y - NECK.y, 0))
    v.co.z += 0.010
    if r.length > 1e-6:
        v.co += r.normalized() * 0.008


edges = extrude_border(bm, top, roll)
edges = extrude_border(bm, edges, roll)
finish(sweater, bm, material("Sweater", (0.25, 0.23, 0.2, 1)), 0.006)
garment_uv(sweater, 'torso')
wear_colour(sweater, lambda p: 0.2)


# --- rubber boots: a modelled shell, swept from the top of the shaft down to the sole ------------
def boot_rings(side):
    """Cross-sections from the sole up: (z, centre x, toe-side y, heel-side y, half width, squareness).
    The heel and toe lines are given directly, so the heel runs straight up into the shaft."""
    feet = [p for p, c in zip(POS, CAT) if c == 'foot' and side_of(p) == side]
    toe_y, heel_y = min(p.y for p in feet), max(p.y for p in feet)
    fx = (min(p.x for p in feet) + max(p.x for p in feet)) / 2
    half_w = (max(p.x for p in feet) - min(p.x for p in feet)) / 2
    ankle, calf = bone('foot_' + side), bone('calf_' + side)
    length = heel_y - toe_y

    def shaft(z):
        return calf.lerp(ankle, (calf.z - z) / (calf.z - ankle.z))
    a = shaft(0.14)
    keys = [
        (-0.014, fx, toe_y - 0.004, heel_y + 0.004, half_w + 0.003, 2.8),   # under the sole, rounded
        (-0.007, fx, toe_y - 0.011, heel_y + 0.010, half_w + 0.010, 2.8),   # the welt round the sole
        (0.003, fx, toe_y - 0.013, heel_y + 0.011, half_w + 0.010, 2.8),
        (0.028, fx, toe_y - 0.012, heel_y + 0.011, half_w + 0.009, 2.6),
        (0.046, fx, toe_y - 0.002, heel_y + 0.011, half_w + 0.008, 2.5),    # the toe cap rounds over
        (0.068, fx, toe_y + 0.30 * length, heel_y + 0.010, half_w + 0.004, 2.4),   # the instep
        (0.100, (fx + a.x) / 2, a.y - 0.070, heel_y + 0.006, 0.053, 2.3),
        (0.140, a.x, a.y - 0.057, heel_y - 0.006, 0.048, 2.2),              # the ankle, narrowest
    ]
    for z, w, fr, bk in ((0.240, 0.055, 0.061, 0.063), (BOOT_TOP - 0.03, 0.063, 0.068, 0.070),
                         (BOOT_TOP, 0.065, 0.070, 0.072), (BOOT_TOP + 0.008, 0.069, 0.074, 0.076)):   # rolled rim
        c = shaft(min(z, BOOT_TOP))
        keys.append((z, c.x, c.y - fr, c.y + bk, w, 2.1))
    return keys


def lerp_keys(keys, z):
    for a, b in zip(keys, keys[1:]):
        if a[0] <= z <= b[0]:
            t = (z - a[0]) / (b[0] - a[0])
            t = t * t * (3 - 2 * t)
            return tuple(x + (y - x) * t for x, y in zip(a, b))
    return keys[-1] if z > keys[-1][0] else keys[0]


def build_boot(side):
    keys = boot_rings(side)
    bm = bmesh.new()
    SEG = 48
    zs = [k[0] for k in keys]
    z = keys[0][0]
    while z < keys[-1][0]:
        z += 0.007 if z < 0.09 else 0.016
        zs.append(min(z, keys[-1][0]))
    zs = sorted(set(round(z, 4) for z in zs))
    def linear(z):
        for a, b in zip(keys, keys[1:]):
            if a[0] <= z <= b[0]:
                t = (z - a[0]) / (b[0] - a[0])
                return [x + (y - x) * t for x, y in zip(a, b)]
        return list(keys[-1] if z > keys[-1][0] else keys[0])
    params = [linear(z) for z in zs]
    for _ in range(6):   # soften the profile along the height, keep the sole and the rim
        params = [params[0]] + [[(a + 2 * b + c) / 4 for a, b, c in zip(params[i - 1], params[i], params[i + 1])]
                                for i in range(1, len(params) - 1)] + [params[-1]]
    rows = []
    for z, prm in zip(zs, params):
        _, cx, fy, by, half, n = prm
        cy = (fy + by) / 2
        front, back = cy - fy, by - cy
        row = []
        for i in range(SEG):
            th = 2 * math.pi * i / SEG
            sx, sy = math.sin(th), -math.cos(th)     # th = 0 points forward (-y)
            ext = front if sy < 0 else back
            r = 1.0 / ((abs(sx) / half) ** n + (abs(sy) / ext) ** n) ** (1.0 / n)
            row.append(bm.verts.new((cx + sx * r, cy + sy * r, z)))
        rows.append(row)
    for a, b in zip(rows, rows[1:]):
        for i in range(SEG):
            bm.faces.new((a[i], a[(i + 1) % SEG], b[(i + 1) % SEG], b[i]))
    bm.faces.new(list(reversed(rows[0])))   # the sole
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    obj = bpy.data.objects.new("Boot_" + side, bpy.data.meshes.new("Boot_" + side))
    bpy.context.collection.objects.link(obj)
    adopt(obj)
    return obj, bm


def toes_to_ball(name):
    return name.replace('bigtoe', 'ball').split('toe')[0] if 'toe' in name else name


boots = []
for s in ('l', 'r'):
    b, bm = build_boot(s)
    for v in bm.verts:
        v.co += noise.noise_vector(v.co * 12.0) * 0.0015
    finish(b, bm, material("Boots", (0.05, 0.06, 0.05, 1)), 0.006)
    skin_from_body(b, lambda i, s=s: CAT[i] in ('calf', 'foot') and side_of(POS[i]) == s,
                   lambda n: ('ball_' + n[-1]) if 'toe' in n else n)
    boots.append(b)
bpy.context.view_layer.objects.active = boots[0]
bpy.ops.object.select_all(action='DESELECT')
for b in boots:
    b.select_set(True)
bpy.ops.object.join()
boots = bpy.context.view_layer.objects.active
boots.name = boots.data.name = "Boots"
garment_uv(boots, 'leg')
wear_colour(boots, lambda p: 0.1 + 0.9 * max(0.0, 1.0 - (p.z + 0.01) / 0.12))
BOOT_INNER = 0.044   # trousers inside the shaft stay within this radius of the shin


# --- trousers, tucked into the boots ------------------------------------------------------------
def in_trousers(i):
    p, c = POS[i], CAT[i]
    return c in ('torso', 'thigh', 'calf') and BOOT_TOP - 0.08 < p.z < HEM + 0.10


trousers, bm = garment_from_body("Trousers", in_trousers)
for s in ('l', 'r'):
    th, ft = bone('thigh_' + s), bone('foot_' + s)
    leg = [v for v in bm.verts if side_of(v.co) == s and v.co.z < CROTCH - 0.01]

    def centre(z, th=th, ft=ft):
        t = (th.z - z) / (th.z - ft.z)
        return th.lerp(ft, t)
    drape(bm, leg, centre, CROTCH - 0.01, lambda ang: 0.85, angles=48, taper=0.12)
    for v in leg:   # gather into the boot
        c = centre(v.co.z)
        d = Vector((v.co.x - c.x, v.co.y - c.y))
        limit = BOOT_INNER + max(0.0, v.co.z - BOOT_TOP) * 0.6
        if d.length > limit and v.co.z < BOOT_TOP + 0.10:
            k = max(0.0, min(1.0, (BOOT_TOP + 0.10 - v.co.z) / 0.10))
            new = d.length + (limit - d.length) * k
            v.co.x, v.co.y = c.x + d.x / d.length * new, c.y + d.y / d.length * new
offset_along_normals(bm, lambda v: 0.010)
smooth(bm, 20, 0.5)
smooth_borders(bm, 8)
bm.normal_update()
for v in bm.verts:   # soft horizontal creases of worn canvas, more at the knees and over the boots
    p = v.co
    knee = math.exp(-((p.z - bone('calf_l').z) / 0.08) ** 2)
    bunch = math.exp(-((p.z - BOOT_TOP - 0.05) / 0.06) ** 2)
    v.co += v.normal * (0.003 + 0.004 * knee + 0.006 * bunch) * noise.noise(Vector((p.x * 6, p.y * 6, p.z * 30)))
finish(trousers, bm, material("Trousers", (0.14, 0.15, 0.17, 1)), 0.006)
garment_uv(trousers, 'leg')
wear_colour(trousers, lambda p: 0.25 + 0.6 * math.exp(-((p.z - bone('calf_l').z) / 0.07) ** 2)
            + 0.4 * max(0.0, (BOOT_TOP + 0.1 - p.z) / 0.1))


# --- the body: hands and wrists only ------------------------------------------------------------
def in_hands(i):
    p, c = POS[i], CAT[i]
    if c == 'hand':
        return True
    return c == 'lowerarm' and arm_param(p, side_of(p)) > 0.78


hands, bm = garment_from_body("Hands", in_hands)
finish(hands, bm, material("Skin", (0.55, 0.4, 0.32, 1)), 0.0)
wear_colour(hands, lambda p: 0.0)

# --- flat cap, fitted to the head ---------------------------------------------------------------
head = preview_head
HP = [head.matrix_world @ v.co for v in head.data.vertices]
HB = bone('head')
top_z = max(p.z for p in HP)
band_z = top_z - 0.088
ring = [p for p in HP if abs(p.z - band_z) < 0.01]
cx = (max(p.x for p in ring) + min(p.x for p in ring)) / 2
cy = (max(p.y for p in ring) + min(p.y for p in ring)) / 2
hx = (max(p.x for p in ring) - min(p.x for p in ring)) / 2 + 0.010
hy = (max(p.y for p in ring) - min(p.y for p in ring)) / 2 + 0.012
print("FISHER_CAP top %.3f band %.3f half %.3f %.3f centre %.3f %.3f" % (top_z, band_z, hx, hy, cx, cy))
mw = head.matrix_world.copy()
head.parent = arm
head.matrix_world = mw
for o in list(bpy.data.objects):
    if o.type == 'ARMATURE' and o is not arm:
        bpy.data.objects.remove(o)
head.modifiers.clear()
head.modifiers.new("Armature", 'ARMATURE').object = arm
head.data.materials.clear()
head.data.materials.append(material("HeadPreview", (0.6, 0.45, 0.38, 1)))

bm = bmesh.new()
SEG, WALL, TOP = 48, 6, 6


def cap_point(i, t):
    """t: 0 at the band round the head, 1 at the edge of the flat top."""
    phi = 2 * math.pi * i / SEG
    front = max(0.0, -math.sin(phi))           # -y is the front
    spread = 1.0 + 0.16 * t ** 1.2 * (0.5 + 0.5 * front)
    forward = 0.05 * t ** 1.2 * (0.2 + front)    # the top is pulled forward over the brim
    # high at the back, almost down on the brim at the front
    edge_z = top_z + 0.004 - (top_z + 0.004 - band_z - 0.014) * front ** 1.3
    return Vector((cx + hx * spread * math.cos(phi),
                   cy + hy * spread * math.sin(phi) - forward,
                   band_z + (edge_z - band_z) * t ** 0.8))


rows = [[bm.verts.new(cap_point(i, j / WALL)) for i in range(SEG)] for j in range(WALL + 1)]
mid = Vector((cx, cy - 0.035, top_z + 0.010))
edge = [v.co.copy() for v in rows[-1]]
for k in range(1, TOP):
    f = 1.0 - k / TOP
    rows.append([bm.verts.new(Vector((mid.x + (q.x - mid.x) * f, mid.y + (q.y - mid.y) * f,
                                      q.z * f + mid.z * (1 - f) + 0.010 * f * (1 - f))))
                 for q in edge])
for a, b in zip(rows, rows[1:]):
    for i in range(SEG):
        bm.faces.new((a[i], a[(i + 1) % SEG], b[(i + 1) % SEG], b[i]))
crown = bm.verts.new(mid)
for i in range(SEG):
    bm.faces.new((rows[-1][i], rows[-1][(i + 1) % SEG], crown))
# brim: the front of the band, out and curving down
idx = [i for i in range(SEG) if math.sin(2 * math.pi * i / SEG) < -0.3]
idx.sort(key=lambda i: math.cos(2 * math.pi * i / SEG))
inner = [rows[0][i] for i in idx]
mid_row, outer = [], []
for i, v in zip(idx, inner):
    phi = 2 * math.pi * i / SEG
    depth = 0.060 * ((-math.sin(phi) - 0.3) / 0.7) ** 0.5
    d = Vector((math.cos(phi) * 0.35, math.sin(phi), 0)).normalized()
    mid_row.append(bm.verts.new(v.co + d * depth * 0.5 + Vector((0, 0, -0.004))))
    outer.append(bm.verts.new(v.co + d * depth + Vector((0, 0, -0.014))))
for row_a, row_b in ((inner, mid_row), (mid_row, outer)):
    for k in range(len(idx) - 1):
        bm.faces.new((row_a[k + 1], row_a[k], row_b[k], row_b[k + 1]))
bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
cap_me = bpy.data.meshes.new("Cap")
cap = bpy.data.objects.new("Cap", cap_me)
bpy.context.collection.objects.link(cap)
adopt(cap)
finish(cap, bm, material("Cap", (0.09, 0.085, 0.075, 1)), 0.004)
cap.vertex_groups.new(name="head").add(list(range(len(cap_me.vertices))), 1.0, 'REPLACE')
cap.modifiers.new("Armature", 'ARMATURE').object = arm
cuv = cap_me.uv_layers.new(name="GarmentUV")
for loop in cap_me.loops:
    p = cap_me.vertices[loop.vertex_index].co
    cuv.data[loop.index].uv = (p.x, p.y)
wear_colour(cap, lambda p: 0.3)

bpy.data.objects.remove(body)
for o in (jacket, sweater, trousers, boots, hands, cap):
    print("FISHER_GARMENT %s verts %d" % (o.name, len(o.data.vertices)))


# --- export -------------------------------------------------------------------------------------
def export(path, objects):
    bpy.ops.object.select_all(action='DESELECT')
    arm.select_set(True)
    for o in objects:
        o.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.export_scene.fbx(filepath=path, use_selection=True, object_types={'ARMATURE', 'MESH'},
                             add_leaf_bones=False, bake_anim=False, mesh_smooth_type='FACE',
                             use_mesh_modifiers=False, colors_type='LINEAR')
    print("FISHER_EXPORT " + path)


os.makedirs(OUT_DIR, exist_ok=True)
export(os.path.join(OUT_DIR, "Fisher_Outfit.fbx"), [hands, jacket, sweater, trousers, boots])
export(os.path.join(OUT_DIR, "Fisher_Cap.fbx"), [cap])

# --- previews: rest pose, then arms forward as when holding the rod --------------------------------
scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'STUDIO'
scene.display.shading.color_type = 'MATERIAL'
scene.render.resolution_x, scene.render.resolution_y = 700, 1000
cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("Cam"))
scene.collection.objects.link(cam)
cam.data.type = 'ORTHO'
scene.camera = cam


def shoot(name, d, height=0.95, scale=2.0, target=None, aspect=(700, 1000)):
    scene.render.resolution_x, scene.render.resolution_y = aspect
    target = target or Vector((0, 0, height))
    cam.data.ortho_scale = scale
    cam.location = target + d * 4
    cam.rotation_euler = (-d).to_track_quat('-Z', 'Y').to_euler()
    scene.render.filepath = PREVIEW + name + ".png"
    bpy.ops.render.render(write_still=True)


shoot("Front", Vector((0, -1, 0)))
shoot("Side", Vector((1, 0, 0)))
shoot("Back", Vector((0, 1, 0)))
shoot("Head", Vector((0.6, -1, 0.15)).normalized(), scale=0.7, target=Vector((0, 0, 1.55)), aspect=(700, 700))
shoot("Hem", Vector((0.5, -1, 0.1)).normalized(), scale=0.5, target=Vector((0.1, 0, 0.8)), aspect=(700, 700))
shoot("Boots", Vector((0.7, -1, 0.3)).normalized(), scale=0.8, target=Vector((0, 0, 0.3)), aspect=(700, 700))


def rotate_world(pb, axis, degrees):
    head = pb.head.copy()
    R = Matrix.Translation(head) @ Matrix.Rotation(math.radians(degrees), 4, axis) @ Matrix.Translation(-head)
    pb.matrix = R @ pb.matrix
    bpy.context.view_layer.update()


bpy.context.view_layer.objects.active = arm
bpy.ops.object.mode_set(mode='POSE')
pb = arm.pose.bones
# arms from the A-pose down to the sides, then both forward to a rod held at the belly
rotate_world(pb['upperarm_r'], Vector((0, 1, 0)), -40)
rotate_world(pb['upperarm_l'], Vector((0, 1, 0)), 40)
rotate_world(pb['upperarm_r'], Vector((1, 0, 0)), -45)
rotate_world(pb['upperarm_l'], Vector((1, 0, 0)), -30)
rotate_world(pb['lowerarm_r'], Vector((1, 0, 0)), -70)
rotate_world(pb['lowerarm_l'], Vector((1, 0, 0)), -95)
rotate_world(pb['thigh_l'], Vector((1, 0, 0)), -30)
rotate_world(pb['calf_l'], Vector((1, 0, 0)), 45)
bpy.ops.object.mode_set(mode='OBJECT')
shoot("PoseShoulders", Vector((0.25, -1, 0.9)).normalized(), scale=0.75, target=Vector((0, 0, 1.4)), aspect=(700, 700))
shoot("PoseFront", Vector((0.4, -1, 0.15)).normalized())
shoot("PoseSide", Vector((1, -0.1, 0.05)).normalized())


dg = bpy.context.evaluated_depsgraph_get()
for o in (jacket, boots, trousers, sweater, cap, hands):
    ev = o.evaluated_get(dg)
    pts = [ev.matrix_world @ v.co for v in ev.data.vertices]
    print("FISHER_BOUNDS %s z %.3f..%.3f x %.3f..%.3f y %.3f..%.3f" % (o.name, min(p.z for p in pts), max(p.z for p in pts),
          min(p.x for p in pts), max(p.x for p in pts), min(p.y for p in pts), max(p.y for p in pts)))
