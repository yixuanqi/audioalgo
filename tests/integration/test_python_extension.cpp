#include "audio_run_service/RunService.h"
#include "audio_io/WavFile.h"
#include "plugins/host/registration.h"
#include "EditorNodeInspector.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <future>
#include <cmath>

namespace {
using namespace audioalgo;
template<class T>T* widget(juce::Component& c,const juce::String& id) {
    if(c.getComponentID()==id)return dynamic_cast<T*>(&c);
    for(auto* child:c.getChildren())if(auto* found=widget<T>(*child,id))return found;return nullptr;
}
RunOutcome background(const RunService& s,const RunRequest& r) {
    juce::ScopedJuceInitialiser_GUI messages;
    auto f=std::async(std::launch::async,[&]{auto value=s.run(r);juce::MessageManager::callAsync([]{juce::MessageManager::getInstance()->stopDispatchLoop();});return value;});
    juce::MessageManager::getInstance()->runDispatchLoop();return f.get();
}
struct Extension {
    std::filesystem::path source=AUDIOALGO_SOURCE_DIR,root=source/"build/runs"/("extension-"+juce::Uuid().toString().toStdString());
    NodeRegistry registry;ProjectModel project;
    Extension() {
        REQUIRE(std::filesystem::create_directories(root));
        std::filesystem::copy(source/"examples/plugins/python_user_transform",root/"plugin",std::filesystem::copy_options::recursive);
        REQUIRE(std::filesystem::copy_file(source/"examples/python_stateful_demo/input.wav",root/"input.wav"));
        REQUIRE(std::filesystem::copy_file(source/"examples/python_extension_demo/coefficient.txt",root/"coefficient.txt"));
        REQUIRE(register_host_nodes(registry,{root/"plugin"}).diagnostics.empty());
        project=std::get<ProjectModel>(load_project_file(source/"examples/python_extension_demo/extension.aaproj"));
        project.nodes[0].parameters["path"]=std::string("input.wav");
    }
    void save(){REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"extension.aaproj",project)));}
    void check(const RunOutcome& result,double gain) {
        const auto* e=std::get_if<Diagnostic>(&result);INFO((e?e->code:"success"));INFO((e?e->message:""));REQUIRE(std::holds_alternative<RunSuccess>(result));
        const auto in=std::get<AudioBuffer>(read_wav_file(root/"input.wav")),out=std::get<AudioBuffer>(read_wav_file(std::get<RunSuccess>(result).output_file));
        REQUIRE(out.frame_count()==1025);REQUIRE(out.channel_count()==2);
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1025;++i) {
            const float main=static_cast<float>(in.channel(c)[i]*static_cast<float>(gain));
            const double expected=static_cast<float>(main+static_cast<float>(-main*.5f));
            CHECK(std::abs(out.channel(c)[i]-expected)<=2e-6+1e-5*std::abs(expected));
        }
    }
};
struct Freeze {std::function<void()> action;};
class FreezeProbe final:public AudioNodeProcessor {
    std::shared_ptr<Freeze> state;
public:
    explicit FreezeProbe(std::shared_ptr<Freeze> s):state(std::move(s)){}
    NodeProcessOutcome process(const NodeProcessRequest&) override{return Diagnostic{"unsupported","block only"};}
    bool supports_block_processing() const noexcept override{return true;}
    std::optional<Diagnostic> prepare(const NodePrepareContext&) override{if(state->action){state->action();state->action={};}return {};}
    AudioBlockOutcome process_block(const AudioBlockView& b) override{for(std::size_t c=0;c<b.channel_count;++c)std::copy_n(b.input_channels[c],b.frame_count,b.output_channels[c]);return AudioBlockResult{b.frame_count};}
};
}

TEST_CASE("P3-06 deployed Python extension six generated parameters and both outputs match independent formula","[P3-06][component]") {
    Extension f;RunService service(f.registry);
    editor::EditorController controller(f.registry);controller.adopt_project(f.project);
    double coefficient=.5;bool enabled=true;int repeat=2;double scale=.25;int polarity=-1;
    for(int profile=0;profile<8;++profile) {
    {
    juce::ScopedJuceInitialiser_GUI messages;
    editor::NodeSelectionState selected;editor::NodeInspector inspector(controller,selected,[]{});inspector.set_descriptor_registry(f.registry);
    const auto set_slider=[&](const char* id,double value){inspector.show_node("python");auto* w=widget<juce::Slider>(inspector,id);REQUIRE(w);w->onDragStart();w->setValue(value,juce::sendNotificationSync);w->onDragEnd();};
    const auto set_text=[&](const char* id,const char* value){inspector.show_node("python");auto* w=widget<juce::TextEditor>(inspector,id);REQUIRE(w);w->setText(value,false);w->onReturnKey();};
        if(profile==1){inspector.show_node("python");auto* w=widget<juce::ToggleButton>(inspector,"parameter.enabled");REQUIRE(w);w->setToggleState(false,juce::dontSendNotification);w->onClick();enabled=false;}
        if(profile==2){inspector.show_node("python");auto* w=widget<juce::ToggleButton>(inspector,"parameter.enabled");REQUIRE(w);w->setToggleState(true,juce::dontSendNotification);w->onClick();enabled=true;set_slider("parameter.repeat",3);repeat=3;}
        if(profile==3){set_slider("parameter.scale",.6);scale=.6;}
        if(profile==4){inspector.show_node("python");auto* w=widget<juce::ComboBox>(inspector,"parameter.polarity");REQUIRE(w);w->setSelectedId(2,juce::dontSendNotification);w->onChange();polarity=1;}
        if(profile==5)set_text("parameter.tag","edited component");
        if(profile==6){REQUIRE(juce::File((f.root/"alternate.txt").string()).replaceWithText("0.25"));set_text("parameter.coefficient","alternate.txt");coefficient=.25;}
        if(profile==7){set_text("parameter.coefficient","coefficient.txt");coefficient=.5;}
    }
        f.project=controller.project();f.save();const auto name="component-"+std::to_string(profile);
        const auto gain=enabled?repeat*scale*polarity*coefficient:1;
        f.check(background(service,{f.project,f.root/"extension.aaproj",f.root/name,127}),gain);
        juce::ChildProcess cli;REQUIRE(cli.start(juce::StringArray{AUDIOALGO_CLI_EXE,"--plugin-dir",(f.root/"plugin").string(),"run",(f.root/"extension.aaproj").string(),(f.root/("cli-"+std::to_string(profile))).string(),"127"}));
        const auto log=cli.readAllProcessOutput();INFO(log.toStdString());REQUIRE(cli.waitForProcessToFinish(10000));REQUIRE(cli.getExitCode()==0);
        const auto actual=std::get<AudioBuffer>(read_wav_file(f.root/("cli-"+std::to_string(profile))/"output.wav"));
        const auto component=std::get<AudioBuffer>(read_wav_file(f.root/name/"output.wav"));for(std::size_t c=0;c<2;++c)CHECK(std::equal(actual.channel(c).begin(),actual.channel(c).end(),component.channel(c).begin()));
    }
}

TEST_CASE("P3-06 resource freezes before native prepare and code update restarts without rebuilding host","[P3-06][snapshot]") {
    Extension f;const auto state=std::make_shared<Freeze>();
    auto d=*f.registry.find("builtin.gain");d.type_id="fixture.freeze_resource";d.parameters.clear();REQUIRE(f.registry.register_node(d,[state]{return std::make_unique<FreezeProbe>(state);}).registered);
    f.project.nodes[1].type_id=d.type_id;f.project.nodes[1].parameters.clear();f.save();RunService service(f.registry);
    state->action=[&]{REQUIRE(juce::File((f.root/"coefficient.txt").string()).replaceWithText("7"));REQUIRE(std::filesystem::remove(f.root/"coefficient.txt"));};
    f.check(background(service,{f.project,f.root/"extension.aaproj",f.root/"frozen-v1",127}),-.25);
    const auto old=juce::JSON::parse(juce::File((f.root/"frozen-v1/run.json").string()));
    REQUIRE(juce::File((f.root/"coefficient.txt").string()).replaceWithText("0.5"));
    const auto script=f.root/"plugin/user_transform.py";const auto original=juce::File(script.string()).loadFileAsString();const auto modified=original.replace("VERSION_GAIN=1.0","VERSION_GAIN=2.0");REQUIRE(original!=modified);REQUIRE(juce::File(script.string()).replaceWithText(modified));
    f.check(background(service,{f.project,f.root/"extension.aaproj",f.root/"updated-v2",127}),-.5);
    const auto next=juce::JSON::parse(juce::File((f.root/"updated-v2/run.json").string()));
    CHECK(old["task_artifacts"][0]["provenance"]["process_id"]!=next["task_artifacts"][0]["provenance"]["process_id"]);
    CHECK(old["task_artifacts"][0]["provenance"]["code_sha256"]!=next["task_artifacts"][0]["provenance"]["code_sha256"]);
    REQUIRE(juce::File((f.root/"coefficient.txt").string()).replaceWithText("invalid coefficient"));
    const auto bad=background(service,{f.project,f.root/"extension.aaproj",f.root/"bad-resource",127});REQUIRE(std::holds_alternative<Diagnostic>(bad));
    CHECK(std::get<Diagnostic>(bad).code=="coefficient_invalid");CHECK(std::get<Diagnostic>(bad).node_id=="python");CHECK(std::get<Diagnostic>(bad).port_id=="coefficient");CHECK_FALSE(std::filesystem::exists(f.root/"bad-resource"));
    REQUIRE(juce::File((f.root/"coefficient.txt").string()).replaceWithText("0.5"));f.check(background(service,{f.project,f.root/"extension.aaproj",f.root/"recovered-v2",127}),-.5);
    CHECK(juce::JSON::parse(juce::File((f.root/"frozen-v1/run.json").string()))["output_sha256"]==old["output_sha256"]);
}
