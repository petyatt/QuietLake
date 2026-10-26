"""Original synthetic one-shot sounds for the life of the pond (no recordings, no dependencies).

A_Plop    - a small fish sipping an insect, a float landing: a short rising "bloop".
A_Bubbles - a trail of bubbles over a rooting crucian: a few soft high blips.
A_Splash  - a fish breaking the surface: an impact, spray hiss, drops falling back.
A_Scatter - fry spraying out of the water: a patter of tiny drops.
A water drop sounds like a small air bubble ringing at a rising pitch, so each "drop" here is a
short sine chirp with a fast decay; splashes add band-limited noise. Mono, 44.1 kHz.
Output: Tools/GeneratedAudio/A_*.wav (imported by Tools/CreateLifeAssets.py).
"""
import math
import os
import random
import wave
from array import array

RATE = 44100
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'GeneratedAudio')


def save(name, buf, peak=0.8):
    top = max(1e-9, max(abs(v) for v in buf))
    fade = int(RATE * 0.003)
    data = array('h', [0]) * len(buf)
    for i, v in enumerate(buf):
        g = min(1.0, i / fade, (len(buf) - 1 - i) / fade)
        data[i] = int(max(-1.0, min(1.0, v / top * peak * g)) * 32767)
    with wave.open(os.path.join(OUT, name + '.wav'), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data.tobytes())


def drop(buf, start, f0, f1, length, amp, decay=0.25):
    """A ringing bubble: sine from f0 to f1 Hz over `length` s, exponential decay."""
    n = int(length * RATE)
    phase = 0.0
    for j in range(n):
        i = start + j
        if i >= len(buf):
            break
        t = j / n
        f = f0 * (f1 / f0) ** min(1.0, t * 1.6)
        phase += math.tau * f / RATE
        attack = min(1.0, j / (RATE * 0.002))
        buf[i] += math.sin(phase) * amp * attack * math.exp(-t / decay)


def noise(buf, rng, start, length, amp, lp, hp, decay):
    """Band-limited noise burst: lp/hp are one-pole coefficients (0..1)."""
    n = int(length * RATE)
    low = band = 0.0
    for j in range(n):
        i = start + j
        if i >= len(buf):
            break
        x = rng.uniform(-1, 1)
        low += (x - low) * lp
        band += (low - band) * hp
        t = j / n
        attack = min(1.0, j / (RATE * 0.004))
        buf[i] += (low - band) * amp * attack * math.exp(-t / decay)


def main():
    os.makedirs(OUT, exist_ok=True)
    rng = random.Random(7)

    plop = [0.0] * int(RATE * 0.45)
    drop(plop, 0, 380, 1100, 0.16, 1.0, 0.3)
    noise(plop, rng, 0, 0.05, 0.25, 0.5, 0.05, 0.3)
    drop(plop, int(RATE * 0.05), 900, 1600, 0.08, 0.25, 0.25)
    save('A_Plop', plop, 0.7)

    bubbles = [0.0] * int(RATE * 2.6)
    t = 0.05
    while t < 2.2:
        f = rng.uniform(700, 1800)
        drop(bubbles, int(t * RATE), f, f * rng.uniform(1.4, 2.0), rng.uniform(0.03, 0.06), rng.uniform(0.3, 1.0), 0.3)
        t += rng.uniform(0.06, 0.35)
    save('A_Bubbles', bubbles, 0.55)

    splash = [0.0] * int(RATE * 1.3)
    noise(splash, rng, 0, 0.12, 1.0, 0.35, 0.02, 0.35)          # the slap on the water
    noise(splash, rng, int(RATE * 0.02), 0.7, 0.55, 0.7, 0.08, 0.3)  # spray hiss
    drop(splash, 0, 160, 420, 0.25, 0.9, 0.35)                   # the cavity closing: a deep bloop
    for _ in range(26):                                           # drops falling back
        s = rng.uniform(0.12, 0.85)
        f = rng.uniform(900, 2600)
        drop(splash, int(s * RATE), f, f * 1.5, rng.uniform(0.02, 0.05), rng.uniform(0.08, 0.3) * (1.1 - s), 0.3)
    save('A_Splash', splash, 0.85)

    scatter = [0.0] * int(RATE * 1.4)
    for _ in range(55):
        s = rng.triangular(0.0, 0.9, 0.15)
        f = rng.uniform(1500, 4200)
        drop(scatter, int(s * RATE), f, f * rng.uniform(1.2, 1.7), rng.uniform(0.012, 0.03), rng.uniform(0.2, 0.8), 0.3)
    noise(scatter, rng, 0, 0.9, 0.12, 0.8, 0.2, 0.35)
    save('A_Scatter', scatter, 0.6)
    print('LIFE_SOUNDS_READY')


main()
