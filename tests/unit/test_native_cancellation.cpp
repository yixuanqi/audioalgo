#include "audio_run_service/RunService.h"
#include "audio_io/WavFile.h"
#include "plugins/builtin/registration.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <cmath>

namespace {
struct Gate {
    std::mutex mutex;std::condition_variable condition;
    bool entered=false,release=true;
    std::atomic<std::size_t> calls{0};
    std::atomic<bool>* cancel_at_end=nullptr;
};
class CancellableFixture final:public audioalgo::AudioNodeProcessor {
public:
    explicit CancellableFixture(std::shared_ptr<Gate> gate):gate_(std::move(gate)){}
    bool supports_block_processing() const noexcept override{return true;}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"owned_unsupported","Block fixture"};
    }
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& b) override {
        if(gate_->calls.fetch_add(1)==0) {
            std::unique_lock lock(gate_->mutex);gate_->entered=true;gate_->condition.notify_all();
            gate_->condition.wait(lock,[&]{return gate_->release;});
        }
        for(std::size_t c=0;c<b.channel_count;++c)std::copy_n(b.input_channels[c],b.frame_count,b.output_channels[c]);
        frames_+=b.frame_count;
        if(gate_->cancel_at_end && frames_>=513)gate_->cancel_at_end->store(true);
        return audioalgo::AudioBlockResult{b.frame_count};
    }
private:
    std::shared_ptr<Gate> gate_;std::size_t frames_=0;
};
}

TEST_CASE("AR-17.b native cancellation stops at a block boundary without success publication then recovers","[AR-17.b][AR-16.d][RUN-03]") {
    juce::ScopedJuceInitialiser_GUI messages;
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("cancel-"+juce::Uuid().toString().toStdString());
    REQUIRE(std::filesystem::create_directories(root));
    audioalgo::AudioBuffer input(16000,{"mono"},513);
    for(std::size_t i=0;i<513;++i)input.mutable_channel(0)[i]=static_cast<float>(0.25*std::sin(i*0.03));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"input.wav",input,audioalgo::WavEncoding::float32)));
    audioalgo::NodeRegistry nodes;REQUIRE(audioalgo::register_builtin_nodes(nodes));
    auto gate=std::make_shared<Gate>();
    audioalgo::NodeDescriptor d{"test.cancel_fixture","1.0.0","Cancellation fixture",
        {{"in","audio.pcm@1",audioalgo::PortDirection::input,true},{"out","audio.pcm@1",audioalgo::PortDirection::output,false}}, {}};
    REQUIRE(nodes.register_node(d,[&]{return std::make_unique<CancellableFixture>(gate);}).registered);
    audioalgo::ProjectModel p;p.project_id="native-cancel";p.selected_output_node_id="output";
    p.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}}},{"node",d.type_id,{}},{"output","builtin.output",{}}};
    p.connections={{"file","audio","node","in"},{"node","out","output","audio"}};
    const auto project_file=root/"cancel.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_file,p)));
    audioalgo::RunService service(nodes);
    auto baseline=service.run({p,project_file,root/"baseline",127});
    REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(baseline));
    const auto baseline_hash=std::get<audioalgo::RunSuccess>(baseline).output_sha256;
    gate=std::make_shared<Gate>();gate->release=false;
    std::atomic<bool> cancel{false};
    audioalgo::RunRequest request{p,project_file,root/"cancelled",127};
    request.is_cancelled=[&]{return cancel.load();};
    bool entered=false;
    auto cancellation_start=std::chrono::steady_clock::now();
    // Prepare the real JUCE graph on the message thread. A separate controller
    // thread requests cancellation while the actual processor is in its block.
    // Blocking the message thread on worker.join would deadlock JUCE preparation.
    std::thread cancelling_controller([&]{
        {std::unique_lock lock(gate->mutex);entered=gate->condition.wait_for(lock,std::chrono::seconds(2),[&]{return gate->entered;});}
        cancellation_start=std::chrono::steady_clock::now();cancel.store(true);
        {std::lock_guard lock(gate->mutex);gate->release=true;}gate->condition.notify_all();
    });
    const auto result=service.run(request);
    cancelling_controller.join();
    REQUIRE(entered);
    CHECK(std::chrono::steady_clock::now()-cancellation_start<=std::chrono::seconds(5));
    CHECK(gate->calls.load()==1);
    CHECK_FALSE(std::filesystem::exists(root/"cancelled/complete.json"));
    CHECK_FALSE(std::filesystem::exists(root/"cancelled"));
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(result));
    CHECK(std::get<audioalgo::Diagnostic>(result).code=="run_cancelled");
    gate=std::make_shared<Gate>();
    auto recovered=service.run({p,project_file,root/"recovered",127});
    REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(recovered));
    CHECK(std::get<audioalgo::RunSuccess>(recovered).output_sha256==baseline_hash);
    CHECK(std::filesystem::is_regular_file(root/"baseline/complete.json"));
    // Cancellation on the final native block must still precede successful publish.
    gate=std::make_shared<Gate>();gate->cancel_at_end=&cancel;cancel.store(false);
    request.output_directory=root/"final-block-cancel";
    auto terminal=service.run(request);
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(terminal));
    CHECK(std::get<audioalgo::Diagnostic>(terminal).code=="run_cancelled");
    CHECK_FALSE(std::filesystem::exists(request.output_directory));
    gate=std::make_shared<Gate>();request.output_directory=root/"pre-cancel";
    auto pre=service.run(request);
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(pre));
    CHECK(std::get<audioalgo::Diagnostic>(pre).code=="run_cancelled");
    CHECK(gate->calls.load()==0);CHECK_FALSE(std::filesystem::exists(request.output_directory));
    auto gain=p;
    gain.nodes[1].type_id="builtin.gain";gain.nodes[1].parameters={{"gain_db",0.0}};
    gain.connections={{"file","audio","node","audio_in"},{"node","audio_out","output","audio"}};
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(root/"gain.aaproj",gain)));
    const auto gain_run=service.run({gain,root/"gain.aaproj",root/"gain-recovery",127});
    REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(gain_run));
    CHECK(std::get<audioalgo::RunSuccess>(gain_run).output_sha256==baseline_hash);
}
