"""Run the shipped example with the real CLI and decode WAV samples independently."""
import argparse, datetime as dt, hashlib, importlib.util, json, math, subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--build-dir',type=Path,required=True)
    args=parser.parse_args()
    example=ROOT/'examples/channel_swap_demo'
    module=importlib.util.spec_from_file_location('swap_oracle',example/'verify_artifacts.py')
    oracle=importlib.util.module_from_spec(module); module.loader.exec_module(oracle)
    fixture=json.loads((example/'demo_manifest.json').read_text(encoding='utf-8'))
    original_hash=sha(example/'input.wav')
    assert original_hash==fixture['input_sha256']
    folder=ROOT/'build/reports/architecture_refactor/channel_swap_demo'/dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    folder.mkdir(parents=True,exist_ok=False)
    executable=args.build_dir.resolve()/'bin/audio_cli.exe'
    command=[str(executable),'run',str(example/'channel_swap_demo.aaproj'),str(folder/'run'),'127']
    started=dt.datetime.now(dt.timezone.utc).isoformat()
    completed=subprocess.run(command,cwd=ROOT,capture_output=True,check=False,timeout=60)
    log=folder/'cli.log'; log.write_bytes(completed.stdout+completed.stderr)
    assert completed.returncode==0, log.read_text(encoding='utf-8',errors='replace')
    source=oracle.read_wav(example/'input.wav')
    output=oracle.read_wav(folder/'run/output.wav')
    errors=[]
    for frame in range(513):
        for channel in range(2):
            expected=source[frame*2+1-channel]
            actual=output[frame*2+channel]
            error=abs(actual-expected)
            assert math.isfinite(actual) and error<=2e-6+1e-5*abs(expected)
            errors.append(error)
    metadata=json.loads((folder/'run/run.json').read_text(encoding='utf-8'))
    complete=json.loads((folder/'run/complete.json').read_text(encoding='utf-8'))
    assert metadata['input_sha256']==original_hash==sha(example/'input.wav')
    assert metadata['output_sha256']==sha(folder/'run/output.wav')==complete['output_sha256']
    assert metadata['run_id']==metadata['request_id']==complete['run_id']
    assert metadata['frames']==513 and metadata['channels']==2 and metadata['sample_rate_hz']==48000
    assert metadata['project_snapshot_sha256']==sha(folder/'run/project.aaproj')
    metrics=[]
    for channel in range(2):
        samples=output[channel::2]; expected=source[1-channel::2]
        peak=max(map(abs,samples)); rms=math.sqrt(math.fsum(x*x for x in samples)/513)
        assert peak==max(map(abs,expected))
        assert abs(rms-math.sqrt(math.fsum(x*x for x in expected)/513))<=1e-12
        metrics.append({'channel_position':channel,'peak':peak,'rms':rms})
    report={'status':'passed','case':'AR-12.e','command':command,'cwd':str(ROOT),
            'started_utc':started,'finished_utc':dt.datetime.now(dt.timezone.utc).isoformat(),
            'exit_code':completed.returncode,'log_path':str(log),'log_sha256':sha(log),
            'executable_sha256':sha(executable),'input_sha256':original_hash,
            'output_sha256':metadata['output_sha256'],'max_absolute_error':max(errors),
            'metrics':metrics,'run_id':metadata['run_id'],'desktop':'not_verified; CLI example evidence only'}
    (folder/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps({'status':'passed','report':str(folder/'report.json'),'max_absolute_error':max(errors)}))
if __name__=='__main__': main()
