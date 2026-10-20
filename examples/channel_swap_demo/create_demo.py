"""Generate a new deterministic stereo PCM16 fixture; refuse to overwrite audio."""
import hashlib, json, math, struct, wave
from pathlib import Path

folder=Path(__file__).resolve().parent
audio=folder/'input.wav'
if audio.exists():
    raise SystemExit('input.wav already exists; preserve it and validate its manifest instead')
samples=[]
for frame in range(513):
    samples.extend((round(7000*math.sin(frame*0.07)), round(6000*math.cos(frame*0.11)+1000)))
with wave.open(str(audio),'wb') as stream:
    stream.setnchannels(2); stream.setsampwidth(2); stream.setframerate(48000)
    stream.writeframes(struct.pack('<'+'h'*len(samples),*samples))
manifest={'sample_rate_hz':48000,'channels':2,'frames':513,'encoding':'PCM16',
          'recipe':'L=round(7000*sin(frame*0.07)); R=round(6000*cos(frame*0.11)+1000)',
          'seed':None,'license':'locally generated analytic fixture; no external recording',
          'input_sha256':hashlib.sha256(audio.read_bytes()).hexdigest(),
          'oracle':'enabled: output[0]=input[1], output[1]=input[0]; disabled: identity; original channel positions/length preserved'}
with (folder/'demo_manifest.json').open('x',encoding='utf-8') as stream:
    json.dump(manifest,stream,indent=2)
print(json.dumps(manifest,indent=2))
