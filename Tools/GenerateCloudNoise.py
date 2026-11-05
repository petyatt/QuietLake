"""Tileable 3D noises for the volumetric clouds (run in Blender, which has numpy):

    blender --background --factory-startup --python Tools/GenerateCloudNoise.py

CloudShape   128^3 RGBA: R Perlin-Worley (the billows), G/B/A Worley at 4, 8 and 16 cells per tile;
CloudErosion  32^3 RGB:  Worley at 4, 8 and 16 cells per tile (frays the edges), A = 1.
Each is written as a strip of its Z slices side by side (128 x 128 tiles -> 16384 x 128 PNG), the
layout UE's volume textures are built from (Tools/ImportCloudNoise.py). Values span 0..1.
"""
import os

import bpy
import numpy as np

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'Tools', 'GeneratedClouds')
os.makedirs(OUT, exist_ok=True)
rng = np.random.default_rng(20261105)


def grid(n):
    a = (np.arange(n, dtype=np.float32) + 0.5) / n
    return np.meshgrid(a, a, a, indexing='ij')     # x, y, z in 0..1


def worley(n, cells):
    """Inverted cellular noise, tileable: 1 at a feature point, 0 far from all of them."""
    x, y, z = grid(n)
    pts = rng.random((cells, cells, cells, 3), dtype=np.float32)
    cx, cy, cz = (np.floor(x * cells).astype(np.int32), np.floor(y * cells).astype(np.int32), np.floor(z * cells).astype(np.int32))
    best = np.full(x.shape, 9.0, dtype=np.float32)
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                ix, iy, iz = cx + dx, cy + dy, cz + dz
                p = pts[ix % cells, iy % cells, iz % cells]
                px = (ix + p[..., 0]) / cells
                py = (iy + p[..., 1]) / cells
                pz = (iz + p[..., 2]) / cells
                d = (x - px) ** 2 + (y - py) ** 2 + (z - pz) ** 2
                np.minimum(best, d, out=best)
    d = np.sqrt(best) * cells        # in cell sizes
    return np.clip(1.0 - d, 0.0, 1.0)


def perlin(n, period):
    """Gradient noise, tileable with the given period (cells per tile), roughly -1..1."""
    x, y, z = grid(n)
    g = rng.normal(size=(period, period, period, 3)).astype(np.float32)
    g /= np.linalg.norm(g, axis=-1, keepdims=True)
    fx, fy, fz = x * period, y * period, z * period
    ix, iy, iz = np.floor(fx).astype(np.int32), np.floor(fy).astype(np.int32), np.floor(fz).astype(np.int32)
    tx, ty, tz = fx - ix, fy - iy, fz - iz
    fade = lambda t: t * t * t * (t * (t * 6 - 15) + 10)
    ux, uy, uz = fade(tx), fade(ty), fade(tz)
    out = 0
    for ox in (0, 1):
        for oy in (0, 1):
            for oz in (0, 1):
                gv = g[(ix + ox) % period, (iy + oy) % period, (iz + oz) % period]
                dot = gv[..., 0] * (tx - ox) + gv[..., 1] * (ty - oy) + gv[..., 2] * (tz - oz)
                w = (ux if ox else 1 - ux) * (uy if oy else 1 - uy) * (uz if oz else 1 - uz)
                out = out + w * dot
    return out


def fbm_perlin(n, base, octaves=4):
    total, amp, norm = 0, 1.0, 0
    for o in range(octaves):
        total = total + amp * perlin(n, base * 2 ** o)
        norm += amp
        amp *= 0.5
    return total / norm


def fbm_worley(n, cells):
    return worley(n, cells) * 0.625 + worley(n, cells * 2) * 0.25 + worley(n, cells * 4) * 0.125


def remap(v, a, b, c, d):
    return c + (v - a) / np.maximum(b - a, 1e-4) * (d - c)


def normalise(v):
    lo, hi = np.percentile(v, 0.5), np.percentile(v, 99.5)
    return np.clip((v - lo) / max(hi - lo, 1e-4), 0, 1)


def save(name, channels, n):
    rgba = np.stack(channels + [np.ones_like(channels[0])] * (4 - len(channels)), axis=-1)   # x, y, z, 4
    # Strip: slice z goes to columns [z*n, (z+1)*n); image rows are y (Blender stores bottom-up).
    strip = np.transpose(rgba, (1, 2, 0, 3)).reshape(n, n * n, 4)      # y, z*x, 4
    img = bpy.data.images.new(name, width=n * n, height=n, alpha=True, float_buffer=False)
    img.colorspace_settings.name = 'Non-Color'
    img.pixels.foreach_set(strip.astype(np.float32).ravel())
    img.filepath_raw = os.path.join(OUT, name + '.png')
    img.file_format = 'PNG'
    img.save()
    print('QUIET_LAKE_CLOUD_NOISE', name, [round(float(c.mean()), 3) for c in channels])


# Shape: Perlin-Worley = Perlin fbm dilated by low-frequency Worley (Schneider 2015).
N = 128
p = normalise(fbm_perlin(N, 4, 4))
w1, w2, w3 = fbm_worley(N, 4), fbm_worley(N, 8), fbm_worley(N, 16)
pw = normalise(np.clip(remap(p, 0.0, 1.0, w1, 1.0), 0, 1))
save('T_CloudShape', [pw, normalise(w1), normalise(w2), normalise(w3)], N)

# Erosion: high-frequency Worley octaves.
M = 32
save('T_CloudErosion', [normalise(fbm_worley(M, 2)), normalise(fbm_worley(M, 4)), normalise(fbm_worley(M, 8))], M)
