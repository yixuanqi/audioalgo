"""Audit the actual R4 before/after join and reject fabricated or stale range evidence."""
from __future__ import annotations
import argparse, datetime as dt, hashlib, json
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
FOLDER=ROOT/'build/reports/loops/RF0006/T18-r4-channel-swap'
CRITICAL=['libs/audio_algorithms/ChannelSwap.h','libs/audio_algorithms/ChannelSwap.cpp',
          'plugins/examples/ChannelSwapNode.h','plugins/examples/ChannelSwapNode.cpp']
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def read(path): return json.loads(path.read_text(encoding='utf-8'))
def differences(before,after):
    return [p for p in sorted(set(before)|set(after)) if before.get(p)!=after.get(p)]
def generic_snapshot():
    paths=[p for folder in (ROOT/'apps/editor',ROOT/'libs') for p in folder.rglob('*') if p.is_file()]
    return {p.relative_to(ROOT).as_posix():sha(p) for p in sorted(paths)
            if p.relative_to(ROOT).as_posix()!='apps/editor/Main.cpp'
            and not p.relative_to(ROOT).as_posix().startswith('libs/audio_algorithms/')}
def record(path,expected_success):
    data=read(path)
    assert (data['exit_code']==0)==expected_success
    assert sha(Path(data['log_path']))==data['log_sha256']
    junit=Path(data['command'][data['command'].index('--output-junit')+1])
    cases=list(ET.parse(junit).iter('testcase'))
    assert cases
    if expected_success:
        assert all(c.find('failure') is None and c.find('error') is None and c.find('skipped') is None for c in cases)
        # Earlier focused records identify the real JUnit command path but did not attach
        # that file as an artifact. Capture its observed hash separately, without editing
        # the old command/source hash or pretending it was rerun now.
        attached=[a for a in data['artifacts'] if Path(a['path'])==junit]
        assert not attached or attached[0].get('sha256')==sha(junit)
        log=Path(data['log_path']).read_text(encoding='utf-8',errors='replace')
        assert all(c.get('name') in log for c in cases)
    else:
        assert any(c.find('failure') is not None for c in cases)
    return data,cases,junit
def self_test():
    assert differences({'a':'one'},{'a':'two'})==['a']
    assert differences({'a':'one'},{'a':'one','new':'two'})==['new']
    assert differences({'a':'one'},{})==['a']
    assert differences({'a':'one'},{'a':'one'})==[]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--capture',action='store_true')
    args=parser.parse_args()
    self_test()
    baseline=FOLDER/'generic-framework-fixed-before.json'
    proof_file=FOLDER/'generic-after.json'
    before=read(baseline)
    initial=read(FOLDER/'generic-before.json')
    assert differences(initial['generic_files'],before['generic_files'])==['libs/audio_juce_backend/JuceGraph.cpp']
    assert before['framework_delta']==['libs/audio_juce_backend/JuceGraph.cpp']
    assert sha(Path(before['framework_green']['path']))==before['framework_green']['sha256']
    framework,_,framework_junit=record(Path(before['framework_green']['path']),True)
    red,red_cases,red_junit=record(Path(before['unregistered_red']['path']),False)
    assert sha(Path(before['unregistered_red']['path']))==before['unregistered_red']['sha256']
    assert all(framework['sources'][p]==digest for p,digest in before['generic_files'].items())
    assert len(red_cases)==1 and red_cases[0].get('name','').startswith('AR-12.a ')
    assert red['sources']==framework['sources']
    green_path=FOLDER/'framework-fixed-join-green.json'
    green,cases,green_junit=record(green_path,True)
    assert any(c.get('name','').startswith('AR-12.a ') for c in cases)
    assert any(c.get('name','').startswith('AR-12.d ') for c in cases)
    if args.capture:
        current=generic_snapshot()
        changed=differences(before['generic_files'],current)
        assert not changed, 'generic product changes during plugin join: '+repr(changed)
        assert all(green['sources'].get(p)==digest for p,digest in current.items())
        proof={'scope':'actual plugin join after separately repaired framework baseline; historical context is retained',
               'captured_utc':dt.datetime.now(dt.timezone.utc).isoformat(),
               'baseline_sha256':sha(baseline),'generic_files':current,
               'critical_files':{p:sha(ROOT/p) for p in CRITICAL},
               'observed_junit_hashes':{str(p):sha(p) for p in (framework_junit,red_junit,green_junit)},
               'junit_hash_scope':'observed during range capture; original execution/source records are unchanged',
               'join_green_record_sha256':sha(green_path),'execution_source_tree_hash':green['source_tree_hash']}
        assert all(green['sources'].get(p)==digest for p,digest in proof['critical_files'].items())
        with proof_file.open('x',encoding='utf-8') as stream: json.dump(proof,stream,indent=2)
    proof=read(proof_file)
    assert proof['baseline_sha256']==sha(baseline)
    assert proof['generic_files']==before['generic_files']
    assert proof['join_green_record_sha256']==sha(green_path)
    assert proof['execution_source_tree_hash']==green['source_tree_hash']
    assert all(sha(Path(p))==digest for p,digest in proof['observed_junit_hashes'].items())
    assert proof['critical_files']=={p:sha(ROOT/p) for p in CRITICAL}, 'algorithm/adapter changed; renew extension evidence'
    # Later independent generic refactors do not rewrite the historical join hash. Current
    # behavior is covered by AR-12.a/c/d; reject algorithm-specific coupling in all current files.
    for path in generic_snapshot():
        if Path(path).suffix in ('.h','.cpp'):
            source=(ROOT/path).read_text(encoding='utf-8')
            assert 'example.cpp_channel_swap' not in source and 'ChannelSwapNodeAdapter' not in source, path
    print(json.dumps({'status':'passed','case':'AR-12.b','generic_files_at_join':len(proof['generic_files']),
        'framework_delta_before_join':before['framework_delta'],'proof':str(proof_file),
        'execution_source_tree_hash':proof['execution_source_tree_hash'],
        'reuse_scope':'unaltered algorithm/adapter and immutable join records; current generic code remains type-independent'}))

if __name__=='__main__': main()
