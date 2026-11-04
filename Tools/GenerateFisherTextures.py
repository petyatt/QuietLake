"""Tileable cloth textures for the angler's clothes, written to Tools/GeneratedFisher:
T_Fisher_Grunge (R, G, B = large, middle and fine blotches), T_Fisher_Canvas_N (plain-weave canvas
normal map) and T_Fisher_Knit_N (knitted stitches normal map).

    blender --background --factory-startup --python Tools/GenerateFisherTextures.py
"""
import os
import struct
import zlib
import numpy as np

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "GeneratedFisher")
os.makedirs(OUT, exist_ok=True)
N = 512
rng = np.random.default_rng(7)
y, x = np.mgrid[0:N, 0:N] / N


def tile_noise(beta, low_cut=1.0):
    """Periodic fractal noise: white noise shaped in the frequency domain, 0..1."""
    f = np.fft.fftfreq(N) * N
    fr = np.sqrt(f[None, :] ** 2 + f[:, None] ** 2)
    shape = np.where(fr < low_cut, 0.0, 1.0 / np.maximum(fr, 1.0) ** beta)
    n = np.real(np.fft.ifft2(np.fft.fft2(rng.standard_normal((N, N))) * shape))
    return (n - n.min()) / (n.max() - n.min())


def normal_map(h, strength):
    """Tangent-space normal map (DirectX green, as UE expects) from a periodic height field."""
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5 * strength
    dy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5 * strength
    n = np.dstack([-dx, dy, np.ones_like(h)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return n * 0.5 + 0.5


def save(name, rgb):
    """8-bit RGB PNG written directly."""
    data = np.clip(np.round(rgb * 255.0), 0, 255).astype(np.uint8)
    raw = b"".join(bytes([0]) + data[r].tobytes() for r in range(N))   # filter type 0 per row

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff)
    png = (bytes([0x89]) + b"PNG" + bytes([13, 10, 26, 10])
           + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    path = os.path.join(OUT, name + ".png")
    with open(path, "wb") as f:
        f.write(png)
    print("FISHER_TEXTURE " + path)


# grunge: three scales of blotches for colour variation, wear and dirt
save("T_Fisher_Grunge", np.dstack([tile_noise(2.2, 1.5), tile_noise(1.6, 3.0), tile_noise(1.0, 12.0)]))

# canvas: plain weave, 40 threads each way per tile; warp and weft pass over and under in turn
T = 40
cx, cy = x * T, y * T
ix, iy = np.floor(cx), np.floor(cy)
warp = np.sin(np.pi * (cx - ix)) * (0.5 + 0.5 * np.sin(np.pi * cy + np.pi * ix))
weft = np.sin(np.pi * (cy - iy)) * (0.5 + 0.5 * np.sin(np.pi * cx + np.pi * iy + np.pi))
canvas = np.maximum(warp, weft) + 0.15 * tile_noise(0.8, 20.0)
save("T_Fisher_Canvas_N", normal_map(canvas, 6.0))

# knit: columns of V-shaped stitches, 10 columns and 14 rows per tile
C, R = 10, 14
u, v = (x * C) % 1.0, (y * R) % 1.0
lean = 0.24 - 0.14 * v
knit = (np.exp(-((u - (0.5 - lean)) ** 2) / 0.018) + np.exp(-((u - (0.5 + lean)) ** 2) / 0.018)) \
    * (0.6 + 0.4 * np.sin(np.pi * v)) + 0.1 * tile_noise(0.8, 20.0)
save("T_Fisher_Knit_N", normal_map(knit, 5.0))
