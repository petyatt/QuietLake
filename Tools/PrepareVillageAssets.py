"""Prepare the user's Meshy FBX archives with Blender's background Python.

Set QUIETLAKE_VILLAGE_SOURCE to the archive directory. Set QUIETLAKE_VILLAGE_ONLY to
comma-separated asset names to prepare a subset. Working files stay in Saved/VillageAssets.
QUIETLAKE_VILLAGE_TEXTURES_ONLY=1 extracts textures without preparing FBX meshes.
Geometry preparation also reprojects all surface maps onto the low mesh.
"""
import math
import os
import shutil
import sys
from array import array
from pathlib import Path, PurePosixPath
from zipfile import ZipFile

import bmesh
import bpy
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree


# Stable archive suffix, longest horizontal dimension (metres), triangle and texture budgets.
ASSETS = {
    'House01': ('143245', 8.5, 145000, 4096),
    'House02': ('143300', 10.0, 145000, 4096),
    'House03': ('143316', 8.7, 145000, 4096),
    'House04': ('143329', 7.8, 145000, 4096),
    'House05': ('143343', 11.0, 145000, 4096),
    'House06': ('143355', 8.0, 145000, 4096),
    'House07': ('143433', 9.5, 145000, 4096),
    'House08': ('143453', 8.3, 145000, 4096),
    'Shop': ('143514', 9.5, 160000, 4096),
    'WoodShed': ('143534', 5.5, 90000, 4096),
    'Cabin': ('143550', 4.7, 90000, 4096),
    'PlankShed': ('143606', 3.4, 65000, 4096),
    'LeanTo': ('143619', 3.2, 65000, 4096),
    'LowShed': ('143634', 3.5, 65000, 4096),
    'FlowerBush': ('143712', 1.8, 7500, 2048),
    'Boulders': ('143739', 2.4, 7500, 2048),
    'Pier': ('143755', 17.0, 30000, 4096),
    'FlatRock': ('143806', 1.7, 5500, 1024),
    'Reeds': ('143819', 1.1, 24000, 1024),
    'Branch': ('143844', 2.5, 6000, 2048),
    'Fence': ('143857', 2.6, 6500, 2048),
    'Gate': ('143908', 1.4, 6000, 1024),
    'RailFence': ('143920', 2.8, 6000, 2048),
    'Clothesline': ('143933', 3.8, 6500, 2048),
    'Firewood': ('143945', 1.8, 50000, 2048),
    'Vegetables': ('144007', 2.9, 65000, 2048),
    'Flowers': ('144027', 1.9, 60000, 2048),
    'Lilies': ('142422', 2.0, 4500, 1024),
}
SCAN_CLEANUP = {
    'FlowerBush': (0.015, 1800),
    'Reeds': (0.008, 2300),
    'Firewood': (0.012, 2200),
    'Vegetables': (0.009, 2000),
    'Flowers': (0.011, 1800),
}


def mesh_bvh(obj):
    return BVHTree.FromObject(obj, bpy.context.evaluated_depsgraph_get())


def clean_foliage(obj, weld_distance, max_parts):
    # Weld dense scan fragments, then discard the smallest disconnected fragments.
    weld = obj.modifiers.new('FoliageWeld', 'WELD')
    weld.merge_threshold = weld_distance
    bpy.ops.object.modifier_apply(modifier=weld.name)
    mesh = bmesh.new()
    mesh.from_mesh(obj.data)
    islands = []
    for vertex in mesh.verts:
        if vertex.tag:
            continue
        vertex.tag = True
        part = [vertex]
        for connected in part:
            for edge in connected.link_edges:
                other = edge.other_vert(connected)
                if not other.tag:
                    other.tag = True
                    part.append(other)
        islands.append(part)
    islands.sort(key=len, reverse=True)
    bmesh.ops.delete(mesh, geom=[v for part in islands[max_parts:] for v in part], context='VERTS')
    mesh.to_mesh(obj.data)
    mesh.free()


def extend_pier_piles(obj, size):
    tree = mesh_bvh(obj)
    deck, _, _, _ = tree.ray_cast(Vector((0, 0, size.z + 1)), Vector((0, 0, -1)))
    if deck is None:
        raise RuntimeError('Pier has no deck on the centre line')
    under_deck = deck.z - 0.28
    stretch = (3.8 - 0.28) / max(under_deck, 0.1)
    for vertex in obj.data.vertices:
        if vertex.co.z < under_deck:
            vertex.co.z = under_deck + (vertex.co.z - under_deck) * stretch
        vertex.co.z -= deck.z
    obj.data.update()


def export_garden_crops(obj, out):
    """Reuse the leaf geometry and UVs of Vegetables for rows planted directly in the ground."""
    image = bpy.data.images.load(str(out / 'Base.png'), check_existing=False)
    width, height = image.size
    pixels = array('f', [0]) * (4 * width * height)
    image.pixels.foreach_get(pixels)
    bpy.data.images.remove(image)

    crop = obj.copy()
    crop.data = obj.data.copy()
    bpy.context.collection.objects.link(crop)
    crop.data.transform(crop.matrix_world)
    crop.matrix_world = Matrix.Identity(4)
    mesh = bmesh.new()
    mesh.from_mesh(crop.data)
    uv_layer = mesh.loops.layers.uv.active

    def is_plant(face):
        z = face.calc_center_median().z
        if z < 0.49:
            return False
        if z > 0.66:
            return True
        uv = sum((loop[uv_layer].uv for loop in face.loops), Vector((0, 0))) / len(face.loops)
        i = 4 * ((int(uv.x * width) % width) + (int(uv.y * height) % height) * width)
        r, g, b = pixels[i:i + 3]
        return g > 0.13 and g > 1.10 * r and g > 1.35 * b

    bmesh.ops.delete(mesh, geom=[f for f in mesh.faces if not is_plant(f)], context='FACES')
    bmesh.ops.delete(mesh, geom=[v for v in mesh.verts if not v.link_faces], context='VERTS')
    if not mesh.faces:
        raise RuntimeError('No crops extracted from Vegetables')
    for vertex in mesh.verts:
        vertex.co.z -= 0.49
    mesh.to_mesh(crop.data)
    mesh.free()
    bpy.ops.object.select_all(action='DESELECT')
    crop.select_set(True)
    bpy.context.view_layer.objects.active = crop
    clean_foliage(crop, 0.007, 1500)
    crop.data.calc_loop_triangles()
    if len(crop.data.loop_triangles) > 30000:
        modifier = crop.modifiers.new('CropsBudget', 'DECIMATE')
        modifier.ratio = 30000 / len(crop.data.loop_triangles)
        bpy.ops.object.modifier_apply(modifier=modifier.name)
    dst = out.parent / 'GardenCrops'
    dst.mkdir(exist_ok=True)
    bpy.ops.export_scene.fbx(filepath=str(dst / 'SM_GardenCrops.fbx'), use_selection=True,
                             object_types={'MESH'}, mesh_smooth_type='FACE',
                             axis_forward='-Y', axis_up='Z', bake_space_transform=True)
    bpy.data.objects.remove(crop, do_unlink=True)


def prepare(name, archive, out, length, budget, resolution, textures_only):
    architectural = name.startswith('House') or name in {'Shop', 'Cabin', 'WoodShed', 'LowShed', 'PlankShed', 'LeanTo'}
    if not textures_only:
        bpy.ops.wm.read_factory_settings(use_empty=True)
    out.mkdir(parents=True, exist_ok=True)
    source = out / 'Source.fbx'
    with ZipFile(archive) as bundle:
        mesh_entries = [item for item in bundle.infolist() if item.filename.lower().endswith('.fbx')]
        if len(mesh_entries) != 1:
            raise RuntimeError(f'{archive.name}: expected one FBX')
        mesh = mesh_entries[0]
        if not textures_only:
            with bundle.open(mesh) as stream, source.open('wb') as dest:
                shutil.copyfileobj(stream, dest)
        for kind, suffix in [('Base', ''), ('Normal', '_normal'), ('Rough', '_roughness')]:
            entry = str(PurePosixPath(mesh.filename).with_suffix('')) + suffix + '.png'
            texture = out / (kind + '.png')
            with bundle.open(entry) as stream, texture.open('wb') as dest:
                shutil.copyfileobj(stream, dest)
            if architectural:
                continue  # Buildings keep the authored texture sizes.
            image = bpy.data.images.load(str(texture), check_existing=False)
            if kind != 'Base':
                image.colorspace_settings.name = 'Non-Color'
            width, height = image.size
            cap = max(512, resolution // 2) if kind == 'Rough' else resolution
            if max(width, height) > cap:
                factor = cap / max(width, height)
                image.scale(round(width * factor), round(height * factor))
                image.save()
            bpy.data.images.remove(image)

    if textures_only:
        print(f'VILLAGE_TEXTURES_PREPARED {name}', flush=True)
        return

    bpy.ops.import_scene.fbx(filepath=str(source), use_image_search=False)
    parts = [obj for obj in bpy.context.scene.objects if obj.type == 'MESH']
    if not parts:
        raise RuntimeError(f'{name}: no mesh')
    bpy.ops.object.select_all(action='DESELECT')
    for obj in parts:
        transform = obj.matrix_world.copy()
        obj.parent = None
        obj.matrix_world = Matrix.Identity(4)
        obj.data.transform(transform)
        obj.select_set(True)
    obj = parts[0]
    bpy.context.view_layer.objects.active = obj
    if len(parts) > 1:
        bpy.ops.object.join()
    source.unlink()
    obj.data.materials.clear()
    obj.data.materials.append(bpy.data.materials.new('VillageSurface'))
    if not obj.data.uv_layers:
        raise RuntimeError(f'{name}: no texture UVs')

    def bounds():
        return [Vector(f(v.co[i] for v in obj.data.vertices) for i in range(3)) for f in (min, max)]

    low, high = bounds()
    size = high - low
    if name in {'Pier', 'Fence', 'RailFence', 'Gate'} and size.y > size.x:
        obj.data.transform(Matrix.Rotation(1.5707963268, 4, 'Z'))
        low, high = bounds()
        size = high - low
    scale = length / max(size.x, size.y)
    origin = Vector(((low.x + high.x) * 0.5, (low.y + high.y) * 0.5, low.z))
    obj.data.transform(Matrix.Scale(scale, 4) @ Matrix.Translation(-origin))
    if name in SCAN_CLEANUP:
        clean_foliage(obj, *SCAN_CLEANUP[name])
    obj.data.calc_loop_triangles()
    original = len(obj.data.loop_triangles)
    if original > budget:
        decimate = obj.modifiers.new('VillageBudget', 'DECIMATE')
        decimate.ratio = budget / original
        bpy.ops.object.modifier_apply(modifier=decimate.name)
    if name == 'Pier':
        extend_pier_piles(obj, size * scale)
    if architectural:
        for face in obj.data.polygons:
            face.use_smooth = True
        obj.data.set_sharp_from_angle(angle=math.radians(50))
    obj.data.calc_loop_triangles()
    if not obj.data.loop_triangles or not obj.data.uv_layers:
        raise RuntimeError(f'{name}: invalid reduced mesh')
    bpy.ops.export_scene.fbx(filepath=str(out / ('SM_' + name + '.fbx')), use_selection=True,
                             object_types={'MESH'}, mesh_smooth_type='EDGE' if architectural else 'FACE',
                             axis_forward='-Y', axis_up='Z', bake_space_transform=True)
    if name == 'Vegetables':
        export_garden_crops(obj, out)
    print(f'VILLAGE_PREPARED {name} {original} -> {len(obj.data.loop_triangles)} tris, '
          f'dimensions {tuple(round(x * scale, 2) for x in size)} m', flush=True)


def main():
    source = Path(os.environ['QUIETLAKE_VILLAGE_SOURCE'])
    out = Path(__file__).resolve().parents[1] / 'Saved' / 'VillageAssets'
    archives = list(source.rglob('*.zip'))
    only = set(filter(None, os.environ.get('QUIETLAKE_VILLAGE_ONLY', '').split(',')))
    textures_only = os.environ.get('QUIETLAKE_VILLAGE_TEXTURES_ONLY') == '1'
    if only - ASSETS.keys():
        raise ValueError(f'Unknown village assets: {only - ASSETS.keys()}')
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from RebakeVillageAssets import rebake, publish_rebaked
    for name, (key, length, budget, resolution) in ASSETS.items():
        if only and name not in only:
            continue
        archive, = (item for item in archives if f'_1109{key}_' in item.name)
        if textures_only and (out / name / 'completed.json').is_file():
            rebake(name, archive)
            publish_rebaked(name)
            if name == 'Vegetables' and (out / 'GardenCrops' / 'completed.json').is_file():
                rebake('GardenCrops', archive)
                publish_rebaked('GardenCrops')
            continue
        (out / name).mkdir(parents=True, exist_ok=True)
        pending = out / name / 'surface_pending'
        pending.write_text('Preparing mesh and matching surface maps')
        prepare(name, archive, out / name, length, budget, resolution, textures_only)
        if textures_only:
            pending.unlink()
            continue
        rebake(name, archive)
        publish_rebaked(name)
        if name == 'Vegetables':
            rebake('GardenCrops', archive)
            publish_rebaked('GardenCrops')


if __name__ == '__main__':
    main()
