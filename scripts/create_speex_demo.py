"""Create a new synthetic Speex example; never overwrite an existing project/audio."""
from pathlib import Path
import argparse
import hashlib
import json
import math
import random
import struct

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--output",type=Path,default=ROOT/"examples/speex_denoise_demo")
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    rng=random.Random(20261022)
    count,rate,channels=32037,16000,2
    pcm=b"".join(struct.pack("<f",0.12*math.sin(2*math.pi*(440 if c==0 else 880)*i/rate)+rng.uniform(-0.08,0.08))
                 for i in range(count) for c in range(channels))
    fmt=struct.pack("<HHIIHH",3,channels,rate,rate*channels*4,channels*4,32)
    chunks=b"fmt "+struct.pack("<I",len(fmt))+fmt+b"data"+struct.pack("<I",len(pcm))+pcm
    audio=args.output/"input.wav"
    audio.write_bytes(b"RIFF"+struct.pack("<I",len(chunks)+4)+b"WAVE"+chunks)
    project={"schema_version":1,"project_id":"speex-denoise-demo","name":"Speex Denoise (synthetic)","revision":1,
             "selected_output_node_id":"output","nodes":[
                 {"id":"file","type_id":"builtin.file_source","parameters":{"path":"input.wav"},"x":30,"y":80},
                 {"id":"denoise","type_id":"speex.denoise","parameters":{"noise_suppress_db":-25},"x":280,"y":80},
                 {"id":"output","type_id":"builtin.output","parameters":{},"x":530,"y":80}],
             "connections":[{"from_node":"file","from_port":"audio","to_node":"denoise","to_port":"audio_in"},
                            {"from_node":"denoise","from_port":"audio_out","to_node":"output","to_port":"audio"}]}
    (args.output/"speex_denoise.aaproj").write_text(json.dumps(project,indent=2)+"\n",encoding="utf-8")
    (args.output/"demo_manifest.json").write_text(json.dumps({"schema_version":1,"generator":"create_speex_demo.py",
        "seed":20261022,"sample_rate_hz":rate,"channels":channels,"frames":count,"input_sha256":hashlib.sha256(audio.read_bytes()).hexdigest(),
        "algorithm":"SpeexDSP 1.2.1, float SmallFT, frame480, AGC/VAD/dereverb off",
        "limitation":"Synthetic tones/noise only; not F04 licensed speech, quality or listening acceptance."},indent=2)+"\n",encoding="utf-8")
    print(args.output.resolve())

if __name__=="__main__":main()
