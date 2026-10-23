#include "audio_run_service/RunService.h"
#include "audio_io/WavFile.h"
#include "plugins/builtin/registration.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {
enum class Fault {none,factory,prepare,process,unknown_process,wav_publish,manifest_publish,readonly_complete};
struct Control {Fault fault=Fault::none;std::filesystem::path run;int live=0;bool obstructed=false;};
class FaultProcessor final:public audioalgo::AudioNodeProcessor {
public:
    explicit FaultProcessor(Control& c):control_(c){++control_.live;}
    ~FaultProcessor() override{--control_.live;}
    bool supports_block_processing() const noexcept override{return true;}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override{return audioalgo::Diagnostic{"owned_unsupported","Borrowed only"};}
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext&) override {
        if(control_.fault==Fault::prepare)throw std::runtime_error("Controlled prepare exception");
        if(control_.fault==Fault::wav_publish || control_.fault==Fault::manifest_publish) {
            const auto block=control_.run/(control_.fault==Fault::wav_publish?"output.wav":"run.json");
            if(!std::filesystem::create_directory(block))return audioalgo::Diagnostic{"fixture_setup_failed","Could not reserve obstructing directory"};
            std::ofstream(block/"owned-sentinel.txt")<<"This is inside a freshly reserved test run";
            control_.obstructed=true;
        }
        if(control_.fault==Fault::readonly_complete) {
            const auto block=control_.run/"complete.json";
            {std::ofstream stream(block);stream<<"incomplete owned test data";if(!stream)return audioalgo::Diagnostic{"fixture_setup_failed","Could not create owned read-only file"};}
            std::filesystem::permissions(block,std::filesystem::perms::owner_read,std::filesystem::perm_options::replace);
            control_.obstructed=true;
        }
        return {};
    }
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& b) override {
        if(control_.fault==Fault::process)throw std::runtime_error("Controlled processing exception");
        if(control_.fault==Fault::unknown_process)throw 42;
        for(std::size_t c=0;c<b.channel_count;++c)std::copy_n(b.input_channels[c],b.frame_count,b.output_channels[c]);
        return audioalgo::AudioBlockResult{b.frame_count};
    }
private:Control& control_;
};
std::string bytes(const std::filesystem::path& p) {std::ifstream stream(p,std::ios::binary);return {std::istreambuf_iterator<char>(stream),{}};}
}

TEST_CASE("AR-16.d exceptions and actual filesystem publication faults leave no success and recover","[AR-16.d][RUN-03]") {
    juce::ScopedJuceInitialiser_GUI messages;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("publish-failure-"+juce::Uuid().toString().toStdString());
    REQUIRE(std::filesystem::create_directories(root));audioalgo::AudioBuffer input(16000,{"mono"},513);
    input.mutable_channel(0)[0]=0.125f;input.mutable_channel(0)[512]=-0.375f;
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"input.wav",input,audioalgo::WavEncoding::float32)));
    audioalgo::NodeRegistry nodes;REQUIRE(audioalgo::register_builtin_nodes(nodes));Control control;
    audioalgo::NodeDescriptor d{"test.publication_fault","1.0.0","Publication fault",
        {{"audio_in","audio.pcm@1",audioalgo::PortDirection::input,true},{"audio_out","audio.pcm@1",audioalgo::PortDirection::output,false}}, {}};
    REQUIRE(nodes.register_node(d,[&]() -> std::unique_ptr<audioalgo::AudioNodeProcessor> {
        if(control.fault==Fault::factory)throw std::runtime_error("Controlled factory exception");
        return std::make_unique<FaultProcessor>(control);
    }).registered);
    audioalgo::ProjectModel p;p.project_id="publication-failures";p.selected_output_node_id="out";
    p.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}}},{"fault",d.type_id,{}},{"out","builtin.output",{}}};
    p.connections={{"file","audio","fault","audio_in"},{"fault","audio_out","out","audio"}};
    const auto file=root/"fault.aaproj";REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(file,p)));
    audioalgo::RunService service(nodes);const auto baseline=service.run({p,file,root/"baseline",127});
    REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(baseline));
    const auto old_output=bytes(root/"baseline/output.wav"),old_report=bytes(root/"baseline/run.json"),old_marker=bytes(root/"baseline/complete.json");
    const auto hash=std::get<audioalgo::RunSuccess>(baseline).output_sha256;
    for(const auto mode:{Fault::factory,Fault::prepare,Fault::process,Fault::unknown_process,Fault::wav_publish,Fault::manifest_publish,Fault::readonly_complete}) {
        CAPTURE(static_cast<int>(mode));control.fault=mode;control.obstructed=false;control.run=root/("failed-"+std::to_string(static_cast<int>(mode)));
        std::optional<audioalgo::RunOutcome> result;
        REQUIRE_NOTHROW(result=service.run({p,file,control.run,127}));REQUIRE(result);
        REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(*result));
        const auto& error=std::get<audioalgo::Diagnostic>(*result);CAPTURE(error.code,error.message);
        CHECK_FALSE(error.message.empty());CHECK(control.live==0);
        if(mode==Fault::factory)CHECK(error.code=="node_factory_threw");
        if(mode==Fault::prepare)CHECK(error.code=="node_prepare_exception");
        if(mode==Fault::process || mode==Fault::unknown_process)CHECK(error.code=="node_process_exception");
        if(mode==Fault::wav_publish)CHECK(error.code=="audio_publish_failed");
        if(mode==Fault::manifest_publish)CHECK(error.code=="run_manifest_write_failed");
        if(mode==Fault::readonly_complete)CHECK(error.code=="run_complete_write_failed");
        if(mode>=Fault::wav_publish)CHECK(control.obstructed);
        CHECK_FALSE(std::filesystem::exists(control.run/"complete.json"));
        CHECK_FALSE(std::filesystem::exists(control.run));
        CHECK(bytes(root/"baseline/output.wav")==old_output);CHECK(bytes(root/"baseline/run.json")==old_report);CHECK(bytes(root/"baseline/complete.json")==old_marker);
        auto gain=p;gain.nodes[1].type_id="builtin.gain";gain.nodes[1].parameters={{"gain_db",0.0}};
        const auto recovery=service.run({gain,file,root/("gain-"+std::to_string(static_cast<int>(mode))),127});
        REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(recovery));
        CHECK(std::get<audioalgo::RunSuccess>(recovery).output_sha256==hash);
    }
}
