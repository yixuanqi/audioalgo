#pragma once
#include "audio_run_service/RunService.h"
#include "audio_io/WavFile.h"
#include "plugins/host/registration.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <future>

namespace audioalgo::test {
struct PythonMixedFault {
    std::filesystem::path root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("mixed-fault-"+juce::Uuid().toString().toStdString());
    std::shared_ptr<NodeRegistry> registry=std::make_shared<NodeRegistry>();ProjectModel project;juce::String code;
    PythonMixedFault() {
        const auto example=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"examples/python_stateful_demo";
        REQUIRE(std::filesystem::create_directories(root));std::filesystem::copy(example/"plugin",root/"plugin",std::filesystem::copy_options::recursive);
        REQUIRE(std::filesystem::copy_file(example/"input.wav",root/"input.wav"));
        auto manifest=juce::JSON::parse(juce::File((root/"plugin/manifest.json").string()));
        for(auto& node:*manifest["nodes"].getArray())node["parameters"].getArray()->add(juce::JSON::parse(R"({"id":"mode","type":"string","default":"ok"})"));
        REQUIRE(juce::File((root/"plugin/manifest.json").string()).replaceWithText(juce::JSON::toString(manifest)));
        code=juce::File((root/"plugin/stateful.py").string()).loadFileAsString()+R"py(
import os,sys,time,subprocess
BaseStateful=Stateful
class Stateful(BaseStateful):
    def process(self,inputs,context):
        mode=context.parameters['mode']
        if mode=='exception':raise RuntimeError('controlled mixed failure')
        if mode=='missing':__import__('audioalgo_missing_dependency_fixture')
        if mode=='crash':os._exit(73)
        if mode in ('wait','spawn'):
            if mode=='spawn':
                child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(1000)'],creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                (context.run_directory/'child-pid.txt').write_text(str(child.pid))
            (context.run_directory/'entered.txt').write_text(str(os.getpid()))
            while True:
                if mode=='wait':context.check_cancelled()
                time.sleep(.005)
        if mode=='flood' and self.calls==0:
            for i in range(256):print('o'*4096);print('e'*4096,file=sys.stderr)
        if mode in ('publish','complete'):
            (context.run_directory.parent/('output.wav' if mode=='publish' else 'complete.json')).mkdir(exist_ok=True)
        result=super().process(inputs,context)
        value=result['main']
        if mode=='dtype':result['main']=AudioPort(value.samples.astype(np.float64),value.sample_rate_hz,value.channel_ids,value.valid_interval)
        if mode=='shape':result['main']=AudioPort(value.samples[:,:-1],value.sample_rate_hz,value.channel_ids,value.valid_interval)
        if mode=='nan':value.samples[0,0]=np.nan
        if mode=='inf':value.samples[0,0]=np.inf
        if mode=='self-modify':
            name=os.path.abspath(__file__)
            if os.name=='nt' and not name.startswith('\\\\?\\'):name='\\\\?\\'+name
            path=Path(name)
            path.write_text(path.read_text(encoding='utf-8')+'\n# changed executed source\n',encoding='utf-8')
        return result
)py";
        REQUIRE(juce::File((root/"plugin/stateful.py").string()).replaceWithText(code));
        REQUIRE(register_host_nodes(*registry,{root/"plugin"}).diagnostics.empty());
        project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"mixed.aaproj",project)));
    }
    RunRequest request(const std::string& name,const std::string& mode) const {
        RunRequest r{project,root/"mixed.aaproj",root/name,127};r.project.nodes[2].parameters["mode"]=mode;return r;
    }
};
inline RunOutcome mixed_worker(const RunService& service,const RunRequest& r) {
    juce::ScopedJuceInitialiser_GUI messages;
    auto future=std::async(std::launch::async,[&]{RunOutcome result;
        try{result=service.run(r);}catch(const std::exception& e){result=Diagnostic{"test_uncaught_exception",e.what()};}
        juce::MessageManager::callAsync([]{juce::MessageManager::getInstance()->stopDispatchLoop();});return result;});
    juce::MessageManager::getInstance()->runDispatchLoop();return future.get();
}
}
