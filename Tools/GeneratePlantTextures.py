"""Bark textures for the plants grown in the Procedural Vegetation Editor, written to Tools/GeneratedPlants.

Both are laid out like the editor's starter bark atlas (T_LeafTree_01_Bark_C), whose columns go from the
oldest bark to the newest shoots: U 0-0.5 the trunk, 0.5-0.763 young trunk and big boughs, 0.763-0.896
branches, 0.896-0.967 twigs, 0.967-1 this year's shoots. In each column U runs round the stem and V along
it; every column tiles on its own. The relief comes from the starter bark's normal map.
T_Birch_Bark_C: the trunk chalk-white with dark horizontal lenticels and black patches, the boughs whiter
and cleaner, the branches grey, the twigs red-brown.
T_Alder_Bark_C: the trunk dark grey-brown, split along the stem into plates, the boughs smoother grey with
pale lenticels, the twigs purple-brown.
Tools/TunePlantMaterials.py imports them into the bark materials.

    blender --background --factory-startup --python Tools/GeneratePlantTextures.py
"""
import os
import struct
import zlib
import numpy as np

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "GeneratedPlants")
os.makedirs(OUT, exist_ok=True)
W = H = 2048
rng = np.random.default_rng(11)


def srgb(r, g, b):
    return np.array([r, g, b], dtype=np.float64) / 255.0


def noise(w, h, beta, low_cut=1.0, stretch=1.0):
    """Periodic fractal noise 0..1 over a w x h tile; stretch > 1 draws the features out along U."""
    fx = np.fft.fftfreq(w) * w * stretch     # higher frequencies round the stem fade faster
    fy = np.fft.fftfreq(h) * h * (w / h)
    fr = np.sqrt(fx[None, :] ** 2 + fy[:, None] ** 2)
    shape = np.where(fr < low_cut, 0.0, 1.0 / np.maximum(fr, 1.0) ** beta)
    n = np.real(np.fft.ifft2(np.fft.fft2(rng.standard_normal((h, w))) * shape))
    return (n - n.min()) / (n.max() - n.min())


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def dashes(w, h, count, length, thick, round_ends=0.5):
    """Mask 0..1 of short horizontal dashes (lenticels) scattered over a tile, wrapping at its edges."""
    mask = np.zeros((h, w))
    for _ in range(count):
        L = rng.uniform(*length)
        T = rng.uniform(*thick)
        u0, v0 = rng.uniform(0, w), rng.uniform(0, h)
        us = np.arange(int(u0 - L / 2), int(u0 + L / 2) + 1)
        vs = np.arange(int(v0 - 2 * T), int(v0 + 2 * T) + 1)
        du = (us - u0) / (L / 2)
        dv = (vs - v0) / T
        across = np.exp(-2.0 * dv ** 2)
        along = np.clip(1.0 - np.abs(du) ** (2.0 / round_ends), 0.0, 1.0) ** 0.5
        # a slightly bent dash: its middle a little higher or lower than its ends
        bend = rng.uniform(-0.6, 0.6) * T * (1 - du ** 2)
        a = np.exp(-2.0 * ((vs[:, None] - v0 - bend[None, :]) / T) ** 2) * along[None, :]
        rows, cols = np.ix_(vs % h, us % w)
        mask[rows, cols] = np.maximum(mask[rows, cols], a * rng.uniform(0.7, 1.0))
    return mask


def birch_trunk(w, h, lenticels, patches, white, grime):
    """White bark: horizontal fibres, grey grime, dark lenticels and black patches."""
    fibres = noise(w, h, 1.2, 4.0, stretch=6.0)            # fine streaks round the stem
    clouds = noise(w, h, 2.4, 1.0, stretch=2.0)            # broad shading
    colour = white[None, None, :] * (0.90 + 0.14 * fibres[:, :, None]) * (0.92 + 0.10 * clouds[:, :, None])
    colour = colour * (1 - grime * smoothstep(0.55, 0.85, clouds))[:, :, None] + srgb(150, 146, 138) * (grime * smoothstep(0.55, 0.85, clouds))[:, :, None]
    # pale orange where the papery outer layer has peeled
    peel = smoothstep(0.80, 0.86, noise(w, h, 2.0, 2.0, stretch=4.0)) * 0.55
    colour = colour * (1 - peel[:, :, None]) + srgb(214, 178, 150) * peel[:, :, None]
    # black patches: lens-shaped bands drawn out round the stem, rough at the edges
    if patches > 0:
        field = noise(w, h, 2.2, 1.5, stretch=3.0) + 0.18 * noise(w, h, 1.0, 8.0, stretch=2.0)
        cut = np.quantile(field, 1.0 - patches)
        dark = smoothstep(cut, cut + 0.035, field)
        black = srgb(38, 34, 32) * (0.8 + 0.4 * fibres[:, :, None])
        colour = colour * (1 - dark[:, :, None]) + black * dark[:, :, None]
    mask = dashes(w, h, *lenticels)
    colour = colour * (1 - 0.88 * mask[:, :, None]) + srgb(46, 40, 38) * (0.88 * mask[:, :, None])
    return colour


def alder_trunk(w, h, base, fissures, lenticels):
    """Dark bark split along the stem: long streaks, darker in the cracks, pale lenticels on young bark."""
    streak = noise(w, h, 1.4, 3.0, stretch=0.3)          # drawn out along the stem
    clouds = noise(w, h, 2.4, 1.0)
    colour = base[None, None, :] * (0.78 + 0.44 * streak[:, :, None]) * (0.9 + 0.2 * clouds[:, :, None])
    crack = smoothstep(0.42, 0.22, streak) * fissures
    colour = colour * (1 - 0.55 * crack[:, :, None])
    if lenticels:
        mask = dashes(w, h, *lenticels)
        colour = colour * (1 - 0.6 * mask[:, :, None]) + srgb(150, 124, 96) * (0.6 * mask[:, :, None])
    return colour


def plain(w, h, base, spread, dots):
    """Branches and twigs: an even colour with fine streaks and light lenticel dots."""
    streak = noise(w, h, 1.3, 3.0, stretch=4.0)
    colour = base[None, None, :] * (0.85 + 0.30 * streak[:, :, None])
    mask = dashes(w, h, *dots)
    return colour * (1 - spread * mask[:, :, None]) + srgb(196, 186, 170) * (spread * mask[:, :, None])


# columns of the atlas (fractions of U)
edges = [0.0, 0.5, 0.763, 0.896, 0.967, 1.0]
cols = [int(round(e * W)) for e in edges]
widths = [cols[i + 1] - cols[i] for i in range(5)]


def atlas(*makers):
    """One maker per column, each called with the column's width and the height."""
    image = np.zeros((H, W, 3))
    for i, make in enumerate(makers):
        image[:, cols[i]:cols[i + 1]] = make(widths[i], H)
    return image


birch = atlas(
    lambda w, h: birch_trunk(w, h, (1500, (14, 70), (1.6, 3.6)), 0.12, srgb(226, 224, 216), 0.35),
    lambda w, h: birch_trunk(w, h, (900, (8, 40), (1.2, 2.6)), 0.05, srgb(232, 230, 223), 0.20),
    lambda w, h: plain(w, h, srgb(118, 108, 100), 0.6, (500, (3, 9), (1.0, 1.8))),
    lambda w, h: plain(w, h, srgb(86, 56, 44), 0.7, (300, (2, 5), (0.8, 1.4))),
    lambda w, h: plain(w, h, srgb(98, 72, 50), 0.5, (120, (2, 4), (0.8, 1.2))))
alder = atlas(
    lambda w, h: alder_trunk(w, h, srgb(70, 64, 60), 1.0, None),
    lambda w, h: alder_trunk(w, h, srgb(92, 86, 78), 0.35, (500, (5, 16), (1.0, 2.0))),
    lambda w, h: plain(w, h, srgb(98, 90, 80), 0.5, (400, (3, 8), (1.0, 1.6))),
    lambda w, h: plain(w, h, srgb(80, 60, 50), 0.6, (250, (2, 5), (0.8, 1.3))),
    lambda w, h: plain(w, h, srgb(94, 82, 54), 0.4, (100, (2, 4), (0.8, 1.2))))


def save(name, rgb):
    """8-bit RGB PNG written directly."""
    data = np.clip(np.round(rgb * 255.0), 0, 255).astype(np.uint8)
    raw = b"".join(bytes([0]) + data[r].tobytes() for r in range(data.shape[0]))   # filter type 0 per row

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff)
    png = (bytes([0x89]) + b"PNG" + bytes([13, 10, 26, 10])
           + chunk(b"IHDR", struct.pack(">IIBBBBB", data.shape[1], data.shape[0], 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    path = os.path.join(OUT, name + ".png")
    with open(path, "wb") as f:
        f.write(png)
    print("PLANT_TEXTURE %s %d bytes" % (path, len(png)))


save("T_Birch_Bark_C", birch)
save("T_Alder_Bark_C", alder)
