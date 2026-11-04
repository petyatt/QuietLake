"""Gives the fish materials already in the project their underwater look (see FishLook.py), without
importing the models again. Materials that have it already get its current code.

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/TuneFishMaterials.py

QUIETLAKE_FISH=Perch,Pike (environment): only these species (default: every M_Meshy_* in /Game/Fish).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unreal
import FishLook

lib = unreal.MaterialEditingLibrary
only = [n.strip() for n in os.environ.get('QUIETLAKE_FISH', '').split(',') if n.strip()]
paths = [p.split('.')[0] for p in unreal.EditorAssetLibrary.list_assets('/Game/Fish', recursive=False)
         if os.path.basename(p.split('.')[0]).startswith('M_Meshy_')]
if only:
    paths = [p for p in paths if os.path.basename(p)[len('M_Meshy_'):] in only]
if not paths:
    raise RuntimeError('No fish materials found')


def input_of(mat, prop, default_out):
    node = lib.get_material_property_input_node(mat, prop)
    if node is None:
        raise RuntimeError('%s: nothing feeds %s' % (mat.get_name(), prop))
    out = default_out
    try:
        out = lib.get_material_property_input_node_output_name(mat, prop) or default_out
    except Exception:
        pass
    return node, out


for path in sorted(paths):
    mat = unreal.load_asset(path)
    if FishLook.has_look(mat):
        FishLook.refresh(mat)
        lib.recompile_material(mat)
        unreal.EditorAssetLibrary.save_asset(path)
        unreal.log('QUIET_LAKE_FISH_LOOK %s: updated' % path)
        continue
    colour, colour_out = input_of(mat, unreal.MaterialProperty.MP_BASE_COLOR, 'RGB')
    rough, rough_out = input_of(mat, unreal.MaterialProperty.MP_ROUGHNESS, 'R')
    FishLook.apply(mat, colour, colour_out, rough, rough_out)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path)
    unreal.log('QUIET_LAKE_FISH_LOOK %s: done (colour from %s.%s, roughness from %s.%s)' % (path, colour.get_name(), colour_out, rough.get_name(), rough_out))
unreal.log('QUIET_LAKE_FISH_LOOK_READY')
