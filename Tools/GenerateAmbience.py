"""Original synthetic ambience, no downloaded recordings or external dependencies."""
import math
import random
import wave
import os
from array import array

RATE = 24000
DURATION = 48
TAU = math.tau
out = os.path.join(os.path.dirname(__file__), 'GeneratedAudio')
os.makedirs(out, exist_ok=True)

def save(name, samples):
    # Gentle fades avoid clicks where the loop wraps.
    for i in range(RATE):
        gain = i / RATE
        samples[i] = int(samples[i] * gain)
        samples[-i-1] = int(samples[-i-1] * gain)
    with wave.open(os.path.join(out,name+'.wav'),'wb') as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(samples.tobytes())

for kind in ('Wind','Water'):
    rng=random.Random(85 if kind=='Wind' else 123)
    samples=array('h')
    low=0.0
    for i in range(RATE*DURATION):
        t=i/RATE
        noise=rng.uniform(-1,1)
        low += (noise-low)*(0.035 if kind=='Wind' else 0.16)
        if kind=='Wind':
            v=low*(0.38+0.16*math.sin(TAU*t/12)+0.07*math.sin(TAU*t/4.8))
        else:
            wash=(0.5+0.5*math.sin(TAU*t/3.0))**3
            v=low*(0.12+0.32*wash)+0.018*wash*math.sin(TAU*(410*t+5*math.sin(t*3)))
        samples.append(int(max(-1,min(1,v))*32767))
    save('A_'+kind,samples)

samples=array('h',[0])*(RATE*DURATION)
rng=random.Random(42)
for group in range(12):
    start=2+group*3.7+rng.uniform(0,0.5)
    for chirp in range(rng.randint(2,4)):
        begin=int((start+chirp*0.23)*RATE)
        duration=rng.uniform(0.09,0.18)
        base=rng.uniform(2200,3500)
        phase=0.0
        for j in range(int(duration*RATE)):
            idx=begin+j
            if idx>=len(samples): break
            u=j/(duration*RATE)
            phase += TAU*(base+800*math.sin(u*math.pi))/RATE
            v=math.sin(math.pi*u)**2*(math.sin(phase)+0.15*math.sin(2*phase))*0.12
            samples[idx]=max(-32767,min(32767,samples[idx]+int(v*32767)))
save('A_Birds',samples)
print('Generated three 48-second ambience loops at 24 kHz.')
