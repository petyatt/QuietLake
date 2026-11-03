"""Original synthetic ambient loops for the places round the lake (no recordings, no dependencies).

A_Brook  - a shallow stream over stones: a soft rush of water and many small ringing bubbles.
A_Frogs  - pond frogs in the warm shallows: raspy croaks in short runs from a few voices, long pauses.
A_Cuckoo - a cuckoo far off in the forest: a run of falling two-note calls, then a long silence.
The loops wrap round seamlessly. Mono, 44.1 kHz.
Output: Tools/GeneratedAudio/A_*.wav (imported by Tools/ImportLakeSounds.py).

    python Tools/GenerateLakeSounds.py
"""
import math
import os
import random
import wave
from array import array

RATE = 44100
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'GeneratedAudio')


def save(name, buf, peak=0.7):
    top = max(1e-9, max(abs(v) for v in buf))
    data = array('h', (int(max(-1.0, min(1.0, v / top * peak)) * 32767) for v in buf))
    with wave.open(os.path.join(OUT, name + '.wav'), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data.tobytes())
    print('LAKE_SOUND', name, '%.1f s' % (len(buf) / RATE))


def wrap_add(buf, start, values):
    """Adds values from start, wrapping round the end so the loop is seamless."""
    n = len(buf)
    for j, v in enumerate(values):
        buf[(start + j) % n] += v


def lowpass(buf, cutoff, passes=1):
    a = 1 - math.exp(-2 * math.pi * cutoff / RATE)
    for _ in range(passes):
        y = buf[-1]
        for _round in range(2):      # twice round, so the filter has settled at the seam
            for i in range(len(buf)):
                y += a * (buf[i] - y)
                buf[i] = y
    return buf


def brook(seconds=24, seed=7):
    rnd = random.Random(seed)
    n = seconds * RATE
    # A soft rush: noise, low-passed, breathing slowly.
    rush = lowpass([rnd.uniform(-1, 1) for _ in range(n)], 900, 2)
    buf = [0.0] * n
    for i in range(n):
        t = i / RATE
        swell = 0.75 + 0.25 * math.sin(2 * math.pi * t / seconds * 3) * math.sin(2 * math.pi * t / seconds * 5 + 1)
        buf[i] = rush[i] * 1.6 * swell
    # Bubbles: short rising chirps, many per second, in little clusters.
    t = 0.0
    while t < seconds:
        t += rnd.expovariate(55)
        f0 = rnd.uniform(380, 1400)
        f1 = f0 * rnd.uniform(1.15, 1.6)
        length = rnd.uniform(0.012, 0.05)
        amp = rnd.uniform(0.04, 0.16)
        m = int(length * RATE)
        phase, values = 0.0, []
        for j in range(m):
            x = j / m
            phase += 2 * math.pi * (f0 + (f1 - f0) * x) / RATE
            values.append(amp * math.sin(phase) * math.exp(-5 * x) * min(1.0, j / 40))
        wrap_add(buf, int(t * RATE), values)
    save('A_Brook', buf, 0.55)


def frogs(seconds=40, seed=11):
    rnd = random.Random(seed)
    n = seconds * RATE
    buf = [0.0] * n
    voices = [(rnd.uniform(380, 520), rnd.uniform(28, 38), rnd.uniform(0.5, 1.0)) for _ in range(4)]
    for pitch, pulse, loud in voices:
        t = rnd.uniform(0, seconds)
        end = t + seconds
        while t < end:
            # A run of 3-9 croaks, then a pause of a few seconds.
            for _ in range(rnd.randint(3, 9)):
                length = rnd.uniform(0.18, 0.34)
                m = int(length * RATE)
                f = pitch * rnd.uniform(0.95, 1.05)
                values = []
                for j in range(m):
                    x = j / m
                    env = math.sin(math.pi * x) ** 0.6
                    rasp = 0.5 + 0.5 * math.sin(2 * math.pi * pulse * j / RATE) ** 3
                    s = math.sin(2 * math.pi * f * j / RATE) + 0.45 * math.sin(4 * math.pi * f * j / RATE) + 0.2 * math.sin(6 * math.pi * f * j / RATE)
                    values.append(0.12 * loud * env * rasp * s)
                wrap_add(buf, int(t * RATE), values)
                t += length + rnd.uniform(0.08, 0.3)
            t += rnd.uniform(2.5, 8.0)
    lowpass(buf, 2500)
    save('A_Frogs', buf, 0.5)


def cuckoo(seconds=75, seed=5):
    rnd = random.Random(seed)
    n = seconds * RATE
    buf = [0.0] * n
    t = 4.0
    for _ in range(rnd.randint(6, 9)):
        hi = rnd.uniform(650, 690)
        lo = hi * 0.83                     # a falling minor third: "ku-koo"
        for k, f in enumerate((hi, lo)):
            length = 0.2 if k == 0 else 0.26
            m = int(length * RATE)
            values, phase = [], 0.0
            for j in range(m):
                x = j / m
                env = min(1.0, x / 0.12) * (1 - x) ** 1.5
                vib = 1 + 0.006 * math.sin(2 * math.pi * 6 * j / RATE)
                phase += 2 * math.pi * f * vib / RATE
                values.append(0.3 * env * (math.sin(phase) + 0.12 * math.sin(2 * phase)))
            start = int((t + (0 if k == 0 else 0.3)) * RATE)
            wrap_add(buf, start, values)
            # A faint echo off the far bank.
            wrap_add(buf, start + int(0.11 * RATE), [0.25 * v for v in values])
        t += rnd.uniform(0.95, 1.1)
    lowpass(buf, 1800)
    save('A_Cuckoo', buf, 0.45)


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    brook()
    frogs()
    cuckoo()
