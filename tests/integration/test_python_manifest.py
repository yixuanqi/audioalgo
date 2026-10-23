"""Discovery must consume data, never execute the plugin it advertises."""
from pathlib import Path
import copy
import json
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'python'))
from audioalgo_runner.manifest import discover

def manifest(plugin='example.python',type_id='example.python_gain'):
    return {'schema_version':1,'runtime':'python','plugin_id':plugin,'plugin_version':'1.0.0','nodes':[{
        'type_id':type_id,'version':'1.0.0','display_name':'Python Gain','entrypoint':'gain_node:GainNode',
        'execution':{'kind':'block','stateful':False,'realtime_verified':False},
        'inputs':[{'id':'audio_in','type':'audio.pcm@1','required':True}],
        'outputs':[{'id':'audio_out','type':'audio.pcm@1'}],
        'format':{'sample_rates_hz':[16000,48000],'channels':[1,2,4]},
        'parameters':[{'id':'gain_db','type':'float','default':0.0,'minimum':-60.0,'maximum':12.0,'unit':'dB','update_policy':'OfflineOnly'}]}]}

class ManifestTests(unittest.TestCase):
    def test_new_directory_discovers_block_clip_and_metadata_without_import(self):
        with tempfile.TemporaryDirectory(prefix='AudioAlgo-python-discovery-') as raw:
            root=Path(raw);package=root/'01-plugin';package.mkdir();m=manifest()
            clip=copy.deepcopy(m['nodes'][0]);clip.update(type_id='example.python_clip',entrypoint='clip_node:ClipNode')
            clip['execution']['kind']='clip';m['nodes'].append(clip)
            (package/'manifest.json').write_text(json.dumps(m),encoding='utf-8')
            marker=root/'side-effect.txt'
            (package/'gain_node.py').write_text(f'from pathlib import Path\nPath({str(marker)!r}).write_text("imported")\nraise RuntimeError("must not execute during scan")\n',encoding='utf-8')
            result=discover([root]);self.assertEqual(result.diagnostics,[])
            self.assertEqual([n.type_id for n in result.nodes],['example.python_gain','example.python_clip'])
            self.assertEqual(result.nodes[0].descriptor['inputs'][0]['id'],'audio_in')
            self.assertEqual(result.nodes[0].descriptor['parameters'][0]['minimum'],-60)
            self.assertEqual(result.nodes[1].descriptor['execution']['kind'],'clip')
            self.assertFalse(marker.exists())
            self.assertEqual(discover([root,package]).nodes,result.nodes)

    def test_invalid_metadata_is_atomic_and_duplicates_never_replace_existing_nodes(self):
        with tempfile.TemporaryDirectory(prefix='AudioAlgo-python-discovery-') as raw:
            root=Path(raw)
            for name,value in [('01-valid',manifest()),('02-duplicate',manifest('example.other'))]:
                p=root/name;p.mkdir();(p/'manifest.json').write_text(json.dumps(value),encoding='utf-8')
            broken=manifest('example.broken','example.must_not_leak');second=copy.deepcopy(broken['nodes'][0]);second['type_id']='example.invalid';second['parameters'][0]['default']=float('nan');broken['nodes'].append(second)
            p=root/'03-invalid';p.mkdir();(p/'manifest.json').write_text(json.dumps(broken),encoding='utf-8')
            result=discover([root]);self.assertEqual([n.type_id for n in result.nodes],['example.python_gain'])
            self.assertEqual([d.code for d in result.diagnostics],['python_node_duplicate','python_manifest_invalid'])

    def test_entrypoints_schema_boolean_numbers_and_path_escape_are_rejected(self):
        for mutation in ('schema','entrypoint','boolean_rate','parameter','port','realtime'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory(prefix='AudioAlgo-python-invalid-') as raw:
                p=Path(raw);m=manifest();n=m['nodes'][0]
                if mutation=='schema':m['schema_version']=2
                if mutation=='entrypoint':n['entrypoint']='../outside:GainNode'
                if mutation=='boolean_rate':n['format']['sample_rates_hz']=[True]
                if mutation=='parameter':n['parameters'][0]['minimum']=float('inf')
                if mutation=='port':n['inputs'].append(copy.deepcopy(n['inputs'][0]))
                if mutation=='realtime':n['execution']['realtime_verified']=True
                (p/'manifest.json').write_text(json.dumps(m),encoding='utf-8')
                result=discover([p]);self.assertEqual(result.nodes,[]);self.assertEqual(len(result.diagnostics),1)

if __name__=='__main__':unittest.main()
