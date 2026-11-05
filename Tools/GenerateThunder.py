"""Synthetic thunder, no recordings or external dependencies (plain python):

    python Tools/GenerateThunder.py

A_ThunderNear - a strike close by: a tearing crack, a heavy bang, then the roll dying away (9 s).
A_ThunderMid  - a few kilometres off: a short crackle and a long rolling rumble (11 s).
A_ThunderFar  - far away: only a low, soft rumble swelling and fading (12 s).
Mono, 24 kHz; the game plays one per strike, delayed by the distance (sound goes 343 m/s).
Import with Tools/ImportLakeSounds.py.
"""
import math
import os
import random
import wave
from array import array

RATE = 24000
OUT = os.path.join(os.path.dirname(__file__), 'GeneratedAudio')
os.makedirs(OUT, exist_ok=True)


def save(name, buf, peak=0.8):
    top = max(1e-9, max(abs(v) for v in buf))
    data = array('h', (int(max(-1.0, min(1.0, v / top * peak)) * 32767) for v in buf))
    with wave.open(os.path.join(OUT, name + '.wav'), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data.tobytes())
    print('QUIET_LAKE_THUNDER', name, round(len(buf) / RATE, 1), 's')


def lowpass(buf, hz):
    a = 1 - math.exp(-math.tau * hz / RATE)
    y, out = 0.0, [0.0] * len(buf)
    for i, v in enumerate(buf):
        y += a * (v - y)
        out[i] = y
    return out


def highpass(buf, hz):
    low = lowpass(buf, hz)
    return [v - l for v, l in zip(buf, low)]


def envelope(n, rng, peaks, decay):
    """Rolling loudness: a few swells (echoes off the clouds and the far shore) over a slow fade."""
    env = [0.0] * n
    for start, height, width in peaks:
        s, w = int(start * RATE), max(1, int(width * RATE))
        for i in range(s, n):
            t = (i - s) / w
            env[i] += height * (1 - math.exp(-t * 6)) * math.exp(-t)
    fade = [math.exp(-i / RATE / decay) for i in range(n)]
    # Slow, irregular flutter: the roll is never a smooth hiss.
    flutter = lowpass([rng.uniform(-1, 1) for _ in range(n)], 3.0)
    top = max(1e-9, max(abs(v) for v in flutter))
    return [e * f * (0.65 + 0.35 * v / top) for e, f, v in zip(env, fade, flutter)]


def rumble(n, rng, cutoff):
    """Deep noise: white noise through two low passes, the energy below ~cutoff Hz."""
    return lowpass(lowpass([rng.uniform(-1, 1) for _ in range(n)], cutoff), cutoff * 1.5)


def crack(n, rng, start, length, count):
    """The tearing crackle of a near strike: dense bursts of bright noise, each a few ms."""
    buf = [0.0] * n
    s0 = int(start * RATE)
    for _ in range(count):
        at = s0 + int(rng.random() ** 1.6 * length * RATE)
        size = int(rng.uniform(0.002, 0.012) * RATE)
        amp = rng.uniform(0.3, 1.0) * math.exp(-(at - s0) / RATE / (length * 0.5))
        for i in range(at, min(n, at + size)):
            buf[i] += amp * rng.uniform(-1, 1) * (1 - (i - at) / size)
    return highpass(buf, 900)


def mix(*layers):
    return [sum(v) for v in zip(*layers)]


def thunder_near(rng):
    n = int(9 * RATE)
    bang_env = [math.exp(-max(0, i / RATE - 0.12) / 0.35) if i / RATE >= 0.12 else 0 for i in range(n)]
    bang = [b * v for b, v in zip(bang_env, rumble(n, rng, 160))]
    roll = rumble(n, rng, 90)
    env = envelope(n, rng, [(0.15, 1.0, 0.9), (1.3, 0.6, 1.4), (2.8, 0.45, 1.8), (4.6, 0.25, 2.0)], 2.6)
    roll = [r * e for r, e in zip(roll, env)]
    sharp = crack(n, rng, 0.0, 0.35, 260)
    return mix([v * 2.2 for v in bang], [v * 1.6 for v in roll], [v * 0.9 for v in sharp])


def thunder_mid(rng):
    n = int(11 * RATE)
    roll = rumble(n, rng, 70)
    env = envelope(n, rng, [(0.3, 0.8, 1.2), (1.6, 1.0, 1.8), (3.6, 0.7, 2.2), (6.0, 0.35, 2.4)], 3.4)
    roll = [r * e for r, e in zip(roll, env)]
    sharp = crack(n, rng, 0.25, 0.6, 70)
    sharp = lowpass(sharp, 2500)
    return mix([v * 1.8 for v in roll], [v * 0.35 for v in sharp])


def thunder_far(rng):
    n = int(12 * RATE)
    roll = rumble(n, rng, 45)
    env = envelope(n, rng, [(0.8, 0.5, 2.0), (2.6, 1.0, 2.6), (5.2, 0.6, 2.8)], 4.0)
    return [r * e for r, e in zip(roll, env)]


if __name__ == '__main__':
    rng = random.Random(20261105)
    save('A_ThunderNear', thunder_near(rng), 0.9)
    save('A_ThunderMid', thunder_mid(rng), 0.8)
    save('A_ThunderFar', thunder_far(rng), 0.7)
