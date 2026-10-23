"""An owned PCM task. All import, state, and file operations run in the worker process."""
import json
from pathlib import Path
import numpy as np
from audioalgo_runner.sdk import AudioPort

class Stateful:
    @classmethod
    def describe(cls):
        return json.loads(Path(__file__).with_name('manifest.json').read_text(encoding='utf-8'))['nodes'][0]

    def prepare(self, context):
        self.position=0
        self.calls=0
        self.prepares=getattr(self,'prepares',0)+1

    def process(self, inputs, context):
        value=inputs['audio_in'];frames=value.samples.shape[1]
        result=value.samples*np.float32(context.parameters['scale'])
        result=result+np.arange(self.position,self.position+frames,dtype=np.float32)*np.float32(context.parameters['step'])
        self.position+=frames;self.calls+=1
        (context.run_directory/'lifecycle.json').write_text(json.dumps({'prepares':self.prepares,'calls':self.calls,'frames':self.position}),encoding='utf-8')
        return {'main':AudioPort(result,value.sample_rate_hz,value.channel_ids,value.valid_interval),
                'side':AudioPort(-result,value.sample_rate_hz,value.channel_ids,value.valid_interval)}

    def reset(self, reason):
        self.position=0

class Clip(Stateful):
    @classmethod
    def describe(cls):
        return json.loads(Path(__file__).with_name('manifest.json').read_text(encoding='utf-8'))['nodes'][1]

    def process_clip(self,inputs,context):
        return self.process(inputs,context)
