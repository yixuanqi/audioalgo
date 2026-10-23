"""Independent plugin folder: only a manifest and Python SDK implementation."""
import json
from pathlib import Path
import numpy as np
from audioalgo_runner.sdk import AudioPort,DiagnosticError

VERSION_GAIN=1.0

class Transform:
    @classmethod
    def describe(cls):
        return json.loads(Path(__file__).with_name('manifest.json').read_text(encoding='utf-8'))['nodes'][0]

    def prepare(self,context):
        try:coefficient=float(Path(context.parameters['coefficient']).read_text(encoding='utf-8'))
        except (ValueError,OSError) as error:raise DiagnosticError('coefficient_invalid',str(error),'coefficient') from error
        if not np.isfinite(coefficient):raise DiagnosticError('coefficient_invalid','Coefficient must be finite','coefficient')
        p=context.parameters
        self.gain=np.float32((p['repeat']*p['scale']*(1 if p['polarity']=='positive' else -1)*coefficient if p['enabled'] else 1)*VERSION_GAIN)
        self.calls=0

    def process(self,inputs,context):
        value=inputs['audio_in'];result=value.samples*self.gain;self.calls+=1
        (context.run_directory/'extension.json').write_text(json.dumps({'calls':self.calls,'tag':context.parameters['tag'],'coefficient':context.parameters['coefficient'],'gain':float(self.gain)}),encoding='utf-8')
        return {'main':AudioPort(result,value.sample_rate_hz,value.channel_ids,value.valid_interval),
                'side':AudioPort(-result,value.sample_rate_hz,value.channel_ids,value.valid_interval)}

    def reset(self,reason):pass
