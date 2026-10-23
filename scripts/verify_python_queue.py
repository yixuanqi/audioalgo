"""Inspect one recorded production queue run and independently read all accepted WAVs."""
import argparse
import datetime as dt
import json
from pathlib import Path
from verify_python_mixed import read_wav,verify,sha

ROOT=Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--command-record',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    command=json.loads(args.command_record.read_text(encoding='utf-8'));assert command['exit_code']==0
    start=dt.datetime.fromisoformat(command['started_utc']).timestamp();end=dt.datetime.fromisoformat(command['finished_utc']).timestamp()
    candidates=[p for p in (ROOT/'build/runs').glob('mixed-fault-*') if start<=p.stat().st_mtime<=end]
    cycles=[p for p in candidates if (p/'queue-evidence.json').is_file()];bursts=[p for p in candidates if (p/'burst-50/complete.json').is_file()]
    assert len(cycles)==len(bursts)==1
    checks=[];root=cycles[0];stats=json.loads((root/'queue-evidence.json').read_text(encoding='utf-8'))
    assert stats['cycles']==50 and stats['successes']==30 and stats['stale_rejected']==15 and stats['views_closed']==5
    assert stats['accepted_errors']==1 and not (root/'cycle-25-crash').exists()
    assert stats['peak_active']<=1 and stats['peak_pending']<=1 and stats['heartbeat_p95_ms']<=100 and stats['heartbeat_max_ms']<=500
    samples=read_wav(root/'input.wav')[1]
    for i in range(50):
        path=root/f'cycle-{i}';cancelled=i%3==0 or i%10==9
        if cancelled:assert not path.exists()
        else:
            report=json.loads((path/'run.json').read_text(encoding='utf-8'));assert report['request_id']==f'cycle-{i}' and report['session_epoch']==i+1
            checks.append(verify(path,samples,.25+i*.005))
    root=bursts[0]
    for i in range(50):assert not (root/f'burst-{i}').exists()
    report=json.loads((root/'burst-50/run.json').read_text(encoding='utf-8'));assert report['request_id']=='burst-50'
    checks.append(verify(root/'burst-50',read_wav(root/'input.wav')[1]))
    with args.output.open('x',encoding='utf-8') as stream:json.dump({'source_command':str(args.command_record),'stats':stats,'stats_sha256':sha(cycles[0]/'queue-evidence.json'),'checks':checks},stream,indent=2)
    print(json.dumps({'verified_wavs':len(checks),'maximum_absolute_error':max(c['maximum_absolute_error'] for c in checks),'stats':stats},indent=2))
    return 0
if __name__=='__main__':raise SystemExit(main())
