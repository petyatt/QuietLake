"""Replace only the close mesh of the four supplied fish, retaining their materials and distant LODs."""
import os
from pathlib import Path

import unreal

source = Path(os.environ['QUIETLAKE_FISH_DETAIL_OUT'])
mesh_editor = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
for species in ('Bream', 'Bleak', 'Roach', 'Rudd'):
    name = f'SM_Fish_Meshy_{species}'
    path = f'/Game/Fish/{name}'
    mesh = unreal.load_asset(path)
    lod_count = mesh_editor.get_lod_count(mesh)
    distant_counts = [mesh.get_num_vertices(i) for i in range(1, lod_count)]
    distances = mesh_editor.get_lod_screen_sizes(mesh)
    material = mesh.get_material(0)
    old_extent = mesh.get_bounds().box_extent
    before = mesh.get_num_vertices(0)
    if mesh_editor.import_lod(mesh, 0, str(source / f'{name}_LOD0.fbx')) != 0:
        raise RuntimeError(f'Unable to import close mesh for {species}')
    mesh.set_material(0, material)
    mesh_editor.set_lod_screen_sizes(mesh, distances)
    unreal.log(unreal.LakeFishTools.mark_fish_fins(mesh))
    after = mesh.get_num_vertices(0)
    assert after > before or after >= 28000, (species, before, after)
    assert mesh_editor.get_lod_count(mesh) == lod_count
    assert [mesh.get_num_vertices(i) for i in range(1, lod_count)] == distant_counts
    assert abs(mesh.get_bounds().box_extent.x - old_extent.x) < .5
    assert unreal.EditorAssetLibrary.save_asset(path)
    unreal.log(f'FISH_DETAIL {species}: close vertices {before} -> {after}; distant vertices {distant_counts}')
