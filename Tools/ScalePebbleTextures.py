"""Shrink the bought pebble textures to game size (the source PNGs are 4K, ~28 MB each).

    python Tools/ScalePebbleTextures.py

Source: Saved/Pebbles/<pack>/*.png  Output: Saved/PebbleAssets/Textures/<Name>_<Map>.png
"""
import io
import os
from pathlib import Path

from PIL import Image

ROOT = Path(os.environ.get('QUIETLAKE_PEBBLES', 'C:/Fish/Saved/Pebbles'))
OUT = Path('C:/Fish/Saved/PebbleAssets/Textures')
OUT.mkdir(parents=True, exist_ok=True)

PACKS = [
    ('pebble_asymmetric', 'Pebble_Asymmetric'),
    ('pebble_flat_oval', 'Pebble_FlatOval'),
    ('pebble_round_chunky', 'Pebble_Round'),
    ('pebble_teardrop', 'Pebble_Teardrop'),
]


def find(pack, suffix):
    for png in pack.rglob('*.png'):
        name = png.stem.lower()
        if suffix and not name.endswith(suffix):
            continue
        if not suffix and any(tail in name for tail in ('_normal', '_roughness', '_metallic')):
            continue
        return png
    raise SystemExit('no %s texture in %s' % (suffix or 'base', pack))


def scale(src, dst, size, normal=False):
    img = Image.open(src).convert('RGB')
    img = img.resize((size, size), Image.LANCZOS)
    img.save(dst, 'PNG', optimize=True)
    return os.path.getsize(dst) // 1024


for folder, name in PACKS:
    pack = next((p for p in ROOT.iterdir() if p.is_dir() and p.name.startswith(folder)), ROOT / folder)
    if not pack.exists():
        print('PEBBLE_TEX skip', folder)
        continue
    sizes = []
    for suffix, map_name, size, normal in (
        ('', 'Base', 512, False),
        ('_normal', 'Normal', 512, True),
        ('_roughness', 'Rough', 256, False),
    ):
        dst = OUT / ('%s_%s.png' % (name, map_name))
        kb = scale(find(pack, suffix), dst, size)
        sizes.append('%s %dx%d %d KB' % (map_name, size, size, kb))
    print('PEBBLE_TEX', name, ', '.join(sizes))

print('PEBBLE_TEXTURES_READY')
