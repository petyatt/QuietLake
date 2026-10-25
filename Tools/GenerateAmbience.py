"""Original synthetic ambience, no downloaded recordings or external dependencies.

A_Wind       - irregular gusts in the trees (no regular swell, so it never sounds like surf).
A_Birds      - a few songbird phrases.
A_RainLight  - drizzle: single drops on leaves, grass and water, a faint patter.
A_RainHeavy  - steady rain to downpour: a dense hiss of countless small impacts.
A_Crickets   - field crickets on a warm night.
All loops are 48 s at 24 kHz, mono; the game mixes the rain layers by the rain intensity.
"""
import math
import random
import wave
import os
from array import array

RATE = 24000
DURATION = 48
N = RATE * DURATION
TAU = math.tau
out = os.path.join(os.path.dirname(__file__), 'GeneratedAudio')
os.makedirs(out, exist_ok=True)


def save(name, values, gain=1.0):
    # Normalise to a common peak and fade the ends so the loop wraps without a click.
    peak = max(1e-9, max(abs(v) for v in values))
    samples = array('h', [0]) * len(values)
    for i, v in enumerate(values):
        samples[i] = int(max(-1.0, min(1.0, v / peak * 0.8 * gain)) * 32767)
    for i in range(RATE):
        g = i / RATE
        samples[i] = int(samples[i] * g)
        samples[-i - 1] = int(samples[-i - 1] * g)
    with wave.open(os.path.join(out, name + '.wav'), 'wb') as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(samples.tobytes())


def smooth_noise(rng, points, length):
    """Slowly varying random curve 0..1 through `points` random values (cosine interpolation)."""
    keys = [rng.random() for _ in range(points + 1)]
    keys[-1] = keys[0]
    curve = [0.0] * length
    for i in range(length):
        x = i / length * points
        k = int(x)
        t = (1 - math.cos((x - k) * math.pi)) / 2
        curve[i] = keys[k] * (1 - t) + keys[k + 1] * t
    return curve


def add_burst(buf, rng, start, length, amp, low_cut, high_cut):
    """A short noise burst, band-limited by two one-pole filters, with a fast decay: one drop."""
    lp = hp = 0.0
    a_hi, a_lo = high_cut, low_cut
    for j in range(length):
        idx = start + j
        if idx >= len(buf):
            break
        x = rng.uniform(-1, 1)
        lp += (x - lp) * a_hi          # removes the harshest highs
        hp += (lp - hp) * a_lo         # tracks the lows, subtracted below
        env = math.exp(-j / (length * 0.22))
        buf[idx] += (lp - hp) * env * amp


# --- wind: brown-ish noise with gusts from a random curve, a little leaf rustle in the gusts ---
rng = random.Random(85)
gusts = smooth_noise(rng, 22, N)
detail = smooth_noise(rng, 140, N)
wind = [0.0] * N
low = rustle = 0.0
for i in range(N):
    x = rng.uniform(-1, 1)
    low += (x - low) * 0.02
    rustle += (x - rustle) * 0.45
    g = 0.25 + 0.75 * gusts[i] ** 1.5
    wind[i] = low * g * (0.8 + 0.4 * detail[i]) + (x - rustle) * 0.05 * g * g
save('A_Wind', wind, 0.5)

# --- birds -----------------------------------------------------------------------------------------
birds = [0.0] * N
rng = random.Random(42)
for group in range(12):
    start = 2 + group * 3.7 + rng.uniform(0, 0.5)
    for chirp in range(rng.randint(2, 4)):
        begin = int((start + chirp * 0.23) * RATE)
        duration = rng.uniform(0.09, 0.18)
        base = rng.uniform(2200, 3500)
        phase = 0.0
        for j in range(int(duration * RATE)):
            idx = begin + j
            if idx >= N:
                break
            u = j / (duration * RATE)
            phase += TAU * (base + 800 * math.sin(u * math.pi)) / RATE
            birds[idx] += math.sin(math.pi * u) ** 2 * (math.sin(phase) + 0.15 * math.sin(2 * phase)) * 0.12
save('A_Birds', birds, 0.2)

# --- light rain: separate drops; on leaves (bright ticks), grass (soft), water (a tiny plop) ---
rng = random.Random(7)
light = [0.0] * N
hiss = 0.0
for i in range(N):                       # a very faint background patter
    x = rng.uniform(-1, 1)
    hiss += (x - hiss) * 0.3
    light[i] = (x - hiss) * 0.015
t = 0.0
while t < DURATION:
    t += rng.expovariate(26.0)           # about 26 drops a second
    start = int(t * RATE)
    kind = rng.random()
    if kind < 0.5:                       # leaf: short and bright
        add_burst(light, rng, start, int(RATE * rng.uniform(0.002, 0.005)), rng.uniform(0.15, 0.5), 0.12, 0.85)
    elif kind < 0.85:                    # grass and soil: duller
        add_burst(light, rng, start, int(RATE * rng.uniform(0.004, 0.009)), rng.uniform(0.1, 0.3), 0.05, 0.35)
    else:                                # water: a soft plop whose pitch rises a little
        amp = rng.uniform(0.05, 0.14)
        f0 = rng.uniform(900, 1700)
        length = int(RATE * 0.03)
        phase = 0.0
        for j in range(length):
            idx = start + j
            if idx >= N:
                break
            u = j / length
            phase += TAU * f0 * (1 + 0.6 * u) / RATE
            light[idx] += math.sin(phase) * math.exp(-u * 6) * amp
save('A_RainLight', light, 0.8)

# --- heavy rain: a dense mass of small impacts over a broad hiss, slowly swelling ---------------
rng = random.Random(9)
heavy = [0.0] * N
swell = smooth_noise(rng, 16, N)
lp = band = 0.0
for i in range(N):
    x = rng.uniform(-1, 1)
    lp += (x - lp) * 0.55
    band += (lp - band) * 0.04
    heavy[i] = (lp - band) * 0.22 * (0.85 + 0.15 * swell[i])
t = 0.0
while t < DURATION:
    t += rng.expovariate(900.0)
    add_burst(heavy, rng, int(t * RATE), int(RATE * rng.uniform(0.0015, 0.004)), rng.uniform(0.02, 0.09), 0.08, 0.7)
save('A_RainHeavy', heavy, 0.9)

# --- night: field crickets, chirps in pulses of three, a few insects at slightly different pitches ---
crickets = [0.0] * N
rng = random.Random(11)
for insect in range(4):
    pitch = rng.uniform(4300, 4900)
    period = rng.uniform(0.55, 0.8)
    t = rng.uniform(0, period)
    gain = rng.uniform(0.5, 1.0)
    while t < DURATION - 0.2:
        for pulse in range(3):
            begin = int((t + pulse * 0.045) * RATE)
            for j in range(int(0.025 * RATE)):
                idx = begin + j
                if idx >= N:
                    break
                u = j / (0.025 * RATE)
                crickets[idx] += math.sin(math.pi * u) * math.sin(TAU * pitch * j / RATE) * gain
        t += period * rng.uniform(0.9, 1.1)
save('A_Crickets', crickets, 0.35)
print('Generated wind, birds, light rain, heavy rain and crickets: 48-second loops at 24 kHz.')
