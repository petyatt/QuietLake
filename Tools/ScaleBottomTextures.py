"""Shrink the bought bottom-prop textures (Meshy, 4K) to game size.

    python Tools/ScaleBottomTextures.py
"""
import io
import os
from pathlib import Path

from PIL import Image

ROOT = Path(os.environ.get('QUIETLAKE_BOTTOM_ASSETS', 'C:/Fish/Saved/BottomAssets'))
OUT = Path('C:/Fish/Saved/BottomAssets/Textures')
OUT.mkdir(parents=True, exist_ok=True)

PACKS = [
    ('sunken_decayed_log', 'Bottom_Log', 1024),
    ('Submerged_Branch_01', 'Bottom_Branch1', 512),
    ('Submerged_Branch_02', 'Bottom_Branch2', 512),
    ('Submerged_Branch_03', 'Bottom_Branch3', 512),
    ('anodonta_shell', 'Bottom_Shell', 256),
    ('crayfish_lowpoly', 'Bottom_Crayfish', 512),
]


def find(pack, suffix):
    for png in pack.rglob('*.png'):
        name = png.stem.lower()
        if suffix and not name.endswith(suffix):
            continue
        if not suffix and any(tail in name for tail in ('_normal', '_roughness', '_metallic')):
            continue
        return png
    return None


def scale(src, dst, size):
    img = Image.open(src).convert('RGB')
    img = img.resize((size, size), Image.LANCZOS)
    img.save(dst, 'PNG', optimize=True)
    return os.path.getsize(dst) // 1024


for prefix, name, size in PACKS:
    pack = next((p for p in ROOT.iterdir() if p.is_dir() and p.name.startswith(prefix)), None)
    if not pack:
        print('BOTTOM_TEX skip', prefix)
        continue
    parts = []
    for suffix, map_name in (('', 'Base'), ('_normal', 'Normal'), ('_roughness', 'Rough')):
        src = find(pack, suffix)
        if not src:
            parts.append('%s none' % map_name)
            continue
        dst = OUT / ('%s_%s.png' % (name, map_name))
        kb = scale(src, dst, size if map_name != 'Rough' else max(128, size // 2))
        parts.append('%s %d KB' % (map_name, kb))
    print('BOTTOM_TEX', name, ', '.join(parts))

print('BOTTOM_TEXTURES_READY')
