"""Wire scanned sand and silt into the lake bed material (M_TerrainScanned).

The underwater part of the base colour stops being a flat tone: the bed samples two scanned
surfaces by world position — sand in the shallows, silt further out — and the existing base colour
(the banks, the clay faces) shows through above the waterline. The sun's caustics are multiplied in
on top. The two masks come from one small Custom node, so nothing is done to the existing custom
node's inputs.

Textures are Poly Haven CC0 (aerial_beach_02, brown_mud_rocks_01).

    UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript \\
        -script=C:/Fish/Tools/WireBedTextures.py
"""
import os
from pathlib import Path

import unreal

lib = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()

SOURCE = Path(os.environ.get('QUIETLAKE_BED_TEXTURES', 'C:/Fish/Saved/BedTextures'))
TEX_DIR = '/Game/Materials/Bed/Textures'
MAT_PATH = '/Game/Materials/M_TerrainScanned'
DIR = '/Game/Materials/Bed'


def node(mat, cls, **props):
    n = lib.create_material_expression(mat, cls)
    for key, value in props.items():
        n.set_editor_property(key, value)
    if 'desc' not in props:
        try:
            n.set_editor_property('desc', 'Bed detail')
        except Exception:  # noqa: BLE001
            pass
    return n


def connect(a, out, b, inp):
    lib.connect_material_expressions(a, out, b, inp)


def import_texture(path, name, srgb, normal=False):
    full = '%s/%s' % (TEX_DIR, name)
    if unreal.EditorAssetLibrary.does_asset_exist(full):
        unreal.EditorAssetLibrary.delete_asset(full)
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', str(path))
    task.set_editor_property('destination_path', TEX_DIR)
    task.set_editor_property('destination_name', name)
    task.set_editor_property('automated', True)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('save', True)
    assets.import_asset_tasks([task])
    tex = unreal.load_asset(full)
    if not tex:
        raise RuntimeError('texture import failed: ' + str(path))
    tex.set_editor_property('srgb', srgb)
    if normal:
        tex.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_NORMALMAP)
        tex.set_editor_property('flip_green_channel', True)
    tex.set_editor_property('lod_group', unreal.TextureGroup.TEXTUREGROUP_WORLD)
    unreal.EditorAssetLibrary.save_asset(tex.get_path_name())
    return tex


sand_dir = next(p for p in SOURCE.iterdir() if p.name.startswith('aerial_beach'))
mud_dir = next(p for p in SOURCE.iterdir() if p.name.startswith('brown_mud'))
tex_sand = import_texture(sand_dir / 'Base.png', 'T_BedSand_Base', True)
tex_sand_n = import_texture(sand_dir / 'Normal.png', 'T_BedSand_Normal', False, normal=True)
tex_mud = import_texture(mud_dir / 'Base.png', 'T_BedMud_Base', True)
tex_mud_n = import_texture(mud_dir / 'Normal.png', 'T_BedMud_Normal', False, normal=True)

mat = unreal.load_asset(MAT_PATH)

# Re-runnable: drop the nodes this script made last time (they all carry a 'Bed' description),
# so it can be run after CreateTerrainMaterial.py rebuilds the material.
for exp in list(lib.get_material_expressions(mat)):
    try:
        desc = exp.get_editor_property('desc')
    except Exception:  # noqa: BLE001
        continue
    if desc and str(desc).startswith('Bed'):
        lib.delete_material_expression(mat, exp)

BASE = None
for exp in lib.get_material_expressions(mat):
    try:
        if exp.get_class().get_name() == 'MaterialExpressionCustom' and 'float3 clay' in exp.get_editor_property('code'):
            BASE = exp
            break
    except Exception:  # noqa: BLE001
        continue
if BASE is None:
    raise SystemExit('base colour custom node not found')

# --- the masks: 0 above water, sand in the shallows, silt deeper ------------------------------
masks = node(mat, unreal.MaterialExpressionCustom, description='Bed masks')
world = node(mat, unreal.MaterialExpressionWorldPosition)
ci = unreal.CustomInput()
ci.set_editor_property('input_name', 'P')
masks.set_editor_property('inputs', [ci])
masks.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
masks.set_editor_property('code', '''
float under = 1 - smoothstep(-32.0, -26.0, P.z);
float depth = max(0.0, -(P.z + 25.0));
float sand = under * (1 - saturate(depth / 200.0));
return float3(under, sand, 1 - sand);
''')
connect(world, '', masks, 'P')

under_mask = node(mat, unreal.MaterialExpressionComponentMask, r=True, g=False, b=False, a=False)
sand_mask = node(mat, unreal.MaterialExpressionComponentMask, r=False, g=True, b=False, a=False)
connect(masks, '', under_mask, '')
connect(masks, '', sand_mask, '')

# --- a world-space UV for the bed textures ----------------------------------------------------
scale = node(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.25, 0.25, 0.0, 1.0))
world_uv = node(mat, unreal.MaterialExpressionMultiply)
connect(world, '', world_uv, 'A')
connect(scale, '', world_uv, 'B')
uv = node(mat, unreal.MaterialExpressionComponentMask, r=True, g=True, b=False, a=False)
connect(world_uv, '', uv, '')

sand = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name='BedSand', texture=tex_sand)
sand.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
connect(uv, '', sand, 'UVs')
mud = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name='BedMud', texture=tex_mud)
mud.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
connect(uv, '', mud, 'UVs')

# --- sand in the shallows over the silt, both over the base colour above water -----------------
bed = node(mat, unreal.MaterialExpressionLinearInterpolate)
connect(mud, 'RGB', bed, 'A')
connect(sand, 'RGB', bed, 'B')
connect(sand_mask, '', bed, 'Alpha')

mixed = node(mat, unreal.MaterialExpressionLinearInterpolate)
connect(BASE, '', mixed, 'A')
connect(bed, '', mixed, 'B')
connect(under_mask, '', mixed, 'Alpha')

# --- caustics on the bed ----------------------------------------------------------------------
caustics = node(mat, unreal.MaterialExpressionCustom, description='Bed caustics')
time = node(mat, unreal.MaterialExpressionTime)
p_in = unreal.CustomInput()
p_in.set_editor_property('input_name', 'P')
t_in = unreal.CustomInput()
t_in.set_editor_property('input_name', 'T')
caustics.set_editor_property('inputs', [p_in, t_in])
caustics.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
caustics.set_editor_property('code', '''
float under = 1 - smoothstep(-32.0, -26.0, P.z);
float depth = max(0.0, -(P.z + 25.0));
float shallow = 1 - saturate(depth / 260.0);
float2 w = P.xy * 0.070;
float t = T * 0.45;
float a = sin(w.x * 1.7 + sin(w.y * 1.3 + t) * 1.6 + t);
float b = sin(w.y * 1.5 + sin(w.x * 1.1 - t * 0.8) * 1.4 - t * 0.7);
float caustic = pow(1.0 - saturate(abs(a * b) * 1.35), 3.0) * under * shallow;
return float3(1.0 + caustic * 0.85, 1.0 + caustic * 0.90, 1.0 + caustic * 0.72);
''')
connect(world, '', caustics, 'P')
connect(time, '', caustics, 'T')

lighted = node(mat, unreal.MaterialExpressionMultiply)
connect(mixed, '', lighted, 'A')
connect(caustics, '', lighted, 'B')
lib.connect_material_property(lighted, '', unreal.MaterialProperty.MP_BASE_COLOR)

lib.recompile_material(mat)
unreal.EditorAssetLibrary.save_loaded_asset(mat)
unreal.log('BED_TEXTURES_WIRED')
