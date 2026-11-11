"""Reproject the Meshy source material onto the prepared village meshes with Blender.

PrepareVillageAssets.py calls this after simplification. It can also reproject existing meshes.
QUIETLAKE_VILLAGE_SOURCE selects the source archives,
and QUIETLAKE_VILLAGE_ONLY optionally selects assets. Output is staged under
Saved/VillageRebaked and can be imported with QUIETLAKE_VILLAGE_PREPARED pointing there.
Each mesh and all three maps are published together, with a completed.json marker.
"""
import json
import math
import os
import shutil
import sys
from pathlib import Path, PurePosixPath
from zipfile import ZipFile

import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

sys.path.insert(0, str(Path(__file__).resolve().parent))
from PrepareVillageAssets import ASSETS, extend_pier_piles


ROOT = Path(__file__).resolve().parents[1]
PREPARED = ROOT / 'Saved' / 'VillageAssets'
OUTPUT = ROOT / 'Saved' / 'VillageRebaked'


def load_mesh(path):
    bpy.ops.import_scene.fbx(filepath=str(path), use_image_search=False)
    parts = [obj for obj in bpy.context.selected_objects if obj.type == 'MESH']
    if not parts:
        raise RuntimeError(f'No mesh in {path}')
    for obj in parts:
        transform = obj.matrix_world.copy()
        obj.parent = None
        obj.matrix_world = Matrix.Identity(4)
        obj.data.transform(transform)
    bpy.context.view_layer.objects.active = parts[0]
    if len(parts) > 1:
        bpy.ops.object.join()
    return parts[0]


def normalize_source(obj, name, length):
    def bounds():
        return [Vector(fn(v.co[i] for v in obj.data.vertices) for i in range(3)) for fn in (min, max)]

    low, high = bounds()
    size = high - low
    if name in {'Pier', 'Fence', 'RailFence', 'Gate'} and size.y > size.x:
        obj.data.transform(Matrix.Rotation(math.pi / 2, 4, 'Z'))
        low, high = bounds()
        size = high - low
    scale = length / max(size.x, size.y)
    origin = Vector(((low.x + high.x) / 2, (low.y + high.y) / 2, low.z))
    obj.data.transform(Matrix.Scale(scale, 4) @ Matrix.Translation(-origin))
    if name == 'Pier':
        extend_pier_piles(obj, size * scale)
    if name == 'GardenCrops':
        obj.data.transform(Matrix.Translation((0, 0, -0.49)))


def align_source(source, target):
    """FBX axis conversion can rotate the prepared mesh by half a turn."""
    tree = BVHTree.FromObject(source, bpy.context.evaluated_depsgraph_get())
    vertices = target.data.vertices
    points = [vertices[i].co for i in range(0, len(vertices), max(1, len(vertices) // 1200))]

    def error(sign):
        distances = sorted(tree.find_nearest(Vector((p.x * sign, p.y * sign, p.z)))[3] for p in points)
        return distances[len(distances) // 2]

    forward, reverse = error(1), error(-1)
    if reverse < forward:
        source.data.transform(Matrix.Rotation(math.pi, 4, 'Z'))
    if min(forward, reverse) > 0.025:
        raise RuntimeError(f'Source alignment error {min(forward, reverse):.4f} m')


def make_source_material(source, folder):
    material = bpy.data.materials.new('SourceSurface')
    material.use_nodes = True
    nodes, links = material.node_tree.nodes, material.node_tree.links
    nodes.clear()
    output = nodes.new('ShaderNodeOutputMaterial')
    emit = nodes.new('ShaderNodeEmission')
    shader = nodes.new('ShaderNodeBsdfPrincipled')
    textures = {}
    for kind in ('Base', 'Normal', 'Rough'):
        texture = nodes.new('ShaderNodeTexImage')
        texture.image = bpy.data.images.load(str(folder / (kind + '.png')))
        if kind != 'Base':
            texture.image.colorspace_settings.name = 'Non-Color'
        textures[kind] = texture
    normal = nodes.new('ShaderNodeNormalMap')
    links.new(textures['Normal'].outputs['Color'], normal.inputs['Color'])
    links.new(normal.outputs['Normal'], shader.inputs['Normal'])
    source.data.materials.clear()
    source.data.materials.append(material)
    for face in source.data.polygons:
        face.material_index = 0
    return material, output, emit, shader, textures


def unwrap(target):
    bpy.ops.object.select_all(action='DESELECT')
    target.select_set(True)
    bpy.context.view_layer.objects.active = target
    while target.data.uv_layers:
        target.data.uv_layers.remove(target.data.uv_layers[0])
    target.data.uv_layers.new(name='SurfaceUV')
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=math.radians(75), margin_method='FRACTION', island_margin=0.0025)
    bpy.ops.object.mode_set(mode='OBJECT')
    # Dense plants can contain more islands than a fixed fractional gutter can
    # fit into one tile. Blender then leaves coordinates outside the bake tile.
    coordinates = np.empty(len(target.data.uv_layers.active.data) * 2, dtype=np.float32)
    target.data.calc_loop_triangles()
    loops = np.empty(len(target.data.loop_triangles) * 3, dtype=np.int32)
    target.data.loop_triangles.foreach_get('loops', loops)
    def used_area():
        target.data.uv_layers.active.data.foreach_get('uv', coordinates)
        triangles = coordinates.reshape(-1, 2)[loops.reshape(-1, 3)]
        a, b = triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0]
        return np.abs(a[:, 0] * b[:, 1] - a[:, 1] * b[:, 0]).sum() * 0.5

    target.data.uv_layers.active.data.foreach_get('uv', coordinates)
    if coordinates.min() < -0.00001 or coordinates.max() > 1.00001 or used_area() < 0.05:
        bpy.ops.object.mode_set(mode='EDIT')
        # Reproject as well: a failed fractional pack can collapse island areas.
        bpy.ops.uv.smart_project(angle_limit=math.radians(75), margin_method='SCALED', island_margin=0.00001)
        bpy.ops.object.mode_set(mode='OBJECT')
    if not target.data.uv_layers.active:
        raise RuntimeError('Missing target UV map')
    target.data.uv_layers.active.data.foreach_get('uv', coordinates)
    if not np.isfinite(coordinates).all() or coordinates.min() < -0.00001 or coordinates.max() > 1.00001:
        raise RuntimeError('Surface UV islands must fit inside the 0–1 bake tile')
    area = used_area()
    if area < 0.05:
        raise RuntimeError(f'Collapsed UV islands: only {area:.4f} of the texture tile is used')


def render_comparison(source, target, source_material, baked_material, folder, length):
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.light = 'FLAT'
    scene.display.shading.color_type = 'TEXTURE'
    scene.display.shading.show_shadows = False
    scene.display.shading.show_cavity = False
    scene.display.shading.show_object_outline = False
    scene.view_settings.view_transform = 'Standard'
    camera = bpy.data.objects.new('MaterialCheck', bpy.data.cameras.new('MaterialCheck'))
    scene.collection.objects.link(camera)
    scene.camera = camera
    camera.data.type = 'ORTHO'
    camera.data.ortho_scale = length * 1.1
    height = max(v.co.z for v in target.data.vertices)
    centre = Vector((0, 0, height * 0.48))
    scene.render.resolution_x = 1000
    scene.render.resolution_y = 850
    scene.render.resolution_percentage = 100
    for side in (-1, 1):
        camera.location = centre + Vector((length * 0.38, length * side * 2, length * 0.38))
        camera.rotation_euler = (centre - camera.location).to_track_quat('-Z', 'Y').to_euler()
        for label, visible, hidden, material in (
            ('Source', source, target, source_material), ('Rebaked', target, source, baked_material)
        ):
            visible.hide_render = False
            hidden.hide_render = True
            material.node_tree.nodes.active = next(
                n for n in material.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image.name.startswith('Base')
            )
            scene.render.filepath = str(folder / f'{label}_{side}.png')
            bpy.ops.render.render(write_still=True)
    source.hide_render = False
    target.hide_render = False


def rebake(name, archive):
    source_name = 'Vegetables' if name == 'GardenCrops' else name
    _, length, _, resolution = ASSETS[source_name]
    folder = OUTPUT / name
    work = folder / 'Working'
    work.mkdir(parents=True, exist_ok=True)
    marker = folder / 'completed.json'
    marker.unlink(missing_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    target = load_mesh(PREPARED / name / ('SM_' + name + '.fbx'))
    original_triangles = sum(len(face.vertices) - 2 for face in target.data.polygons)
    with ZipFile(archive) as bundle:
        mesh_entry, = (entry for entry in bundle.namelist() if entry.lower().endswith('.fbx'))
        with bundle.open(mesh_entry) as src, (work / 'Source.fbx').open('wb') as dst:
            shutil.copyfileobj(src, dst)
        for kind, suffix in (('Base', ''), ('Normal', '_normal'), ('Rough', '_roughness')):
            entry = str(PurePosixPath(mesh_entry).with_suffix('')) + suffix + '.png'
            with bundle.open(entry) as src, (work / (kind + '.png')).open('wb') as dst:
                shutil.copyfileobj(src, dst)
    source = load_mesh(work / 'Source.fbx')
    normalize_source(source, name, length)
    align_source(source, target)
    source_material, output, emit, shader, textures = make_source_material(source, work)
    unwrap(target)

    material = bpy.data.materials.new('VillageSurface')
    material.use_nodes = True
    target.data.materials.clear()
    target.data.materials.append(material)
    for face in target.data.polygons:
        face.material_index = 0
    image_node = material.node_tree.nodes.new('ShaderNodeTexImage')
    material.node_tree.nodes.active = image_node

    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = 4
    scene.render.bake.use_selected_to_active = True
    scene.render.bake.margin = 16
    scene.render.bake.margin_type = 'EXTEND'
    scene.render.bake.cage_extrusion = 0.025
    scene.render.bake.max_ray_distance = 0.12
    scene.render.bake.normal_space = 'TANGENT'
    scene.view_settings.view_transform = 'Standard'
    bpy.ops.object.select_all(action='DESELECT')
    source.select_set(True)
    target.select_set(True)
    bpy.context.view_layer.objects.active = target
    baked_images = {}
    for kind in ('Base', 'Normal', 'Rough'):
        size = resolution if kind != 'Rough' else max(512, resolution // 2)
        image = bpy.data.images.new('Baked' + kind, width=size, height=size, alpha=False)
        if kind != 'Base':
            image.colorspace_settings.name = 'Non-Color'
        image_node.image = image
        links = source_material.node_tree.links
        if kind == 'Normal':
            links.new(shader.outputs['BSDF'], output.inputs['Surface'])
        else:
            links.new(textures[kind].outputs['Color'], emit.inputs['Color'])
            links.new(emit.outputs['Emission'], output.inputs['Surface'])
        bpy.ops.object.bake(type='NORMAL' if kind == 'Normal' else 'EMIT')
        if kind == 'Base':
            pixels = np.empty(len(image.pixels), dtype=np.float32)
            image.pixels.foreach_get(pixels)
            if pixels.reshape(-1, 4)[:, :3].max() < 0.001:
                raise RuntimeError(f'{name}: empty albedo bake; surface set was not published')
        image.filepath_raw = str(work / (kind + '.png.baked.png'))
        image.file_format = 'PNG'
        image.save()
        baked_images[kind] = image
        print(f'VILLAGE_BAKE_MAP {name} {kind} {size}', flush=True)

    # Export only the low mesh. Its UV and tangent basis must travel with all three maps.
    bpy.ops.object.select_all(action='DESELECT')
    target.select_set(True)
    bpy.context.view_layer.objects.active = target
    bpy.ops.export_scene.fbx(filepath=str(work / ('SM_' + name + '.fbx')), use_selection=True,
                             object_types={'MESH'}, mesh_smooth_type='FACE', use_tspace=True,
                             axis_forward='-Y', axis_up='Z', bake_space_transform=True)
    image_node.image = baked_images['Base']
    textures['Base'].image.name = 'BaseSource'
    baked_images['Base'].name = 'BaseBaked'
    render_comparison(source, target, source_material, material, folder, length)
    for kind in ('Base', 'Normal', 'Rough'):
        (work / (kind + '.png.baked.png')).replace(folder / (kind + '.png'))
    (work / ('SM_' + name + '.fbx')).replace(folder / ('SM_' + name + '.fbx'))
    marker.write_text(json.dumps(dict(asset=name, triangles=original_triangles,
                                     normal_convention='OpenGL', rebaked=True), indent=2))
    print(f'VILLAGE_REBAKED {name} {original_triangles}', flush=True)


def main():
    archive_folder = Path(os.environ['QUIETLAKE_VILLAGE_SOURCE'])
    archives = list(archive_folder.rglob('*.zip'))
    only = set(filter(None, os.environ.get('QUIETLAKE_VILLAGE_ONLY', '').split(',')))
    names = list(ASSETS) + ['GardenCrops']
    if only - set(names):
        raise ValueError(f'Unknown assets {only - set(names)}')
    for name in names:
        if only and name not in only:
            continue
        suffix = ASSETS['Vegetables' if name == 'GardenCrops' else name][0]
        archive, = (p for p in archives if f'_1109{suffix}_' in p.name)
        rebake(name, archive)


def publish_rebaked(name):
    """Publish the mesh and its maps as one guarded set for the normal importer."""
    source, destination = OUTPUT / name, PREPARED / name
    files = ['SM_' + name + '.fbx', 'Base.png', 'Normal.png', 'Rough.png', 'completed.json']
    for filename in files:
        if not (source / filename).is_file():
            raise RuntimeError(f'{name}: incomplete rebaked surface')
    destination.mkdir(parents=True, exist_ok=True)
    pending = destination / 'surface_pending'
    pending.write_text('Publishing mesh and matching surface maps')
    for filename in files:
        temporary = destination / (filename + '.partial')
        shutil.copyfile(source / filename, temporary)
        temporary.replace(destination / filename)
    pending.unlink()


if __name__ == '__main__':
    main()
