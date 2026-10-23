#include "EditorRunQueue.h"
#include "audio_editor_application/RunReception.h"
#include "audio_io/WavFile.h"
#include "audio_project/ProjectFileIO.h"
#include "plugins/builtin/registration.h"
#include "plugins/builtin/GainNodeAdapter.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <thread>
#include <cmath>

namespace {
struct QueueProbe {
    std::atomic<bool> entered{false},release{false};
    std::mutex mutex;std::condition_variable cv;
    std::vector<double> prepared;
};
class QueuedGain final:public audioalgo::AudioNodeProcessor {
public:
    explicit QueuedGain(std::shared_ptr<QueueProbe> p):probe_(std::move(p)){}
    bool supports_block_processing() const noexcept override{return true;}
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext& c) override {
        probe_->prepared.push_back(std::get<double>(c.parameters->at("gain_db")));
        return gain_.prepare(c);
    }
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& b) override {
        if(!waited_ && probe_->prepared.size()==1) {
            waited_=true;probe_->entered.store(true);
            std::unique_lock lock(probe_->mutex);probe_->cv.wait(lock,[&]{return probe_->release.load();});
        }
        return gain_.process_block(b);
    }
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest& r) override{return gain_.process(r);}
    void reset() noexcept override{gain_.reset();}
private:
    std::shared_ptr<QueueProbe> probe_;audioalgo::GainNodeAdapter gain_;bool waited_=false;
};
struct QueueTarget final:juce::Component {
    audioalgo::RunReception reception;
    std::optional<audioalgo::RunOutcome> accepted;
    int deliveries=0,stale=0;
};
}

TEST_CASE("AR-17.a production run queue retains one active and only the latest pending snapshot","[AR-17.a]") {
    juce::ScopedJuceInitialiser_GUI messages;
    auto nodes=std::make_shared<audioalgo::NodeRegistry>();REQUIRE(audioalgo::register_builtin_nodes(*nodes));
    auto probe=std::make_shared<QueueProbe>();
    auto descriptor=*nodes->find("builtin.gain");descriptor.type_id="test.queued_gain";
    REQUIRE(nodes->register_node(descriptor,[probe]{return std::make_unique<QueuedGain>(probe);}).registered);
    const auto root=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/("latest-"+juce::Uuid().toString().toStdString());
    REQUIRE(std::filesystem::create_directories(root));
    audioalgo::AudioBuffer input(16000,{"mono"},513);
    for(std::size_t i=0;i<513;++i)input.mutable_channel(0)[i]=static_cast<float>(0.125*std::sin(i*0.04));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(audioalgo::write_wav_file(root/"input.wav",input,audioalgo::WavEncoding::float32)));
    audioalgo::ProjectModel project;project.project_id="latest-queue";project.selected_output_node_id="output";
    project.nodes={{"file","builtin.file_source",{{"path",std::string("input.wav")}}},{"gain",descriptor.type_id,{{"gain_db",0.0}}},{"output","builtin.output",{}}};
    project.connections={{"file","audio","gain","audio_in"},{"gain","audio_out","output","audio"}};
    const auto path=root/"latest.aaproj";REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(path,project)));
    QueueTarget target;
    auto dispatch=std::make_shared<audioalgo::editor::ComponentDispatchState<QueueTarget>>(target);
    audioalgo::editor::EditorWorkerPool pool(1);
    audioalgo::editor::EditorRunQueue<QueueTarget> queue(pool,dispatch,nodes,std::make_shared<audioalgo::ProjectResourceService>(),
        [](QueueTarget& t,audioalgo::RunOutcome result,audioalgo::editor::WaveformChannels,audioalgo::RunIdentity identity) {
            const auto& current=t.reception.active();
            if(current && t.reception.accepts(identity,current->project_id,current->session_epoch,current->revision)) {
                ++t.deliveries;t.accepted=std::move(result);
            } else ++t.stale;
        });
    const auto submit=[&](int index) {
        auto p=project;p.revision=index;
        p.nodes[1].parameters["gain_db"]=index==0?0.0:-6.0+12.0*index/50.0;
        audioalgo::RunIdentity id{p.project_id,1,"request-"+std::to_string(index),p.revision};
        target.reception.begin(id);
        audioalgo::RunRequest request{p,path,root/("run-"+std::to_string(index)),127};request.identity=id;
        return queue.submit(std::move(request),id);
    };
    REQUIRE(submit(0));
    bool submitted=false,all_accepted=true,timeout=false,closed=false;
    int peak_jobs=0;
    struct Drive final:juce::Timer {
        std::function<void()> step;
        void timerCallback() override{step();}
    } drive;
    const auto started=std::chrono::steady_clock::now();
    drive.step=[&]{
        if(probe->entered.load() && !submitted) {
            for(int i=1;i<=50;++i)all_accepted=submit(i)&&all_accepted;
            peak_jobs=pool.get_num_jobs();submitted=true;
            probe->release.store(true);probe->cv.notify_all();
        }
        const auto exceeded=std::chrono::steady_clock::now()-started>std::chrono::seconds(5);
        if(!closed && ((target.accepted && pool.get_num_jobs()==0)||exceeded)) {
            timeout=exceeded;closed=true;drive.stopTimer();probe->release.store(true);probe->cv.notify_all();
            const auto stopping=pool.shutdown_async(5000,[](bool){juce::MessageManager::getInstance()->stopDispatchLoop();});
            if(!stopping)juce::MessageManager::getInstance()->stopDispatchLoop();
        }
    };
    drive.startTimer(2);juce::MessageManager::getInstance()->runDispatchLoop();drive.stopTimer();pool.join_shutdown();
    REQUIRE_FALSE(timeout);REQUIRE(submitted);CHECK(all_accepted);
    CHECK(peak_jobs<=2);CHECK(probe->prepared.size()==2);
    CHECK(target.stale==1);
    CHECK_FALSE(std::filesystem::exists(root/"run-0"));
    REQUIRE(target.accepted);REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(*target.accepted));
    const auto& success=std::get<audioalgo::RunSuccess>(*target.accepted);
    CHECK(success.identity.request_id=="request-50");CHECK(target.deliveries==1);
    const auto rendered=audioalgo::read_wav_file(success.output_file);REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(rendered));
    const auto& output=std::get<audioalgo::AudioBuffer>(rendered);
    for(std::size_t i=0;i<513;++i) REQUIRE(std::abs(output.channel(0)[i]-input.channel(0)[i]*std::pow(10.0,6.0/20.0))<1e-6);
    for(int i=1;i<50;++i) CHECK_FALSE(std::filesystem::exists(root/("run-"+std::to_string(i))));
}

TEST_CASE("AR-17.c closing a queued render releases its reservation before the worker starts","[AR-17.c][AR-17.a]") {
    juce::ScopedJuceInitialiser_GUI messages;
    auto nodes=std::make_shared<audioalgo::NodeRegistry>();REQUIRE(audioalgo::register_builtin_nodes(*nodes));
    auto probe=std::make_shared<QueueProbe>();
    struct Blocker final:juce::ThreadPoolJob {
        std::shared_ptr<QueueProbe> probe;
        explicit Blocker(std::shared_ptr<QueueProbe> p):ThreadPoolJob("controlled file work"),probe(std::move(p)){}
        JobStatus runJob() override {
            probe->entered.store(true);
            std::unique_lock lock(probe->mutex);probe->cv.wait(lock,[&]{return probe->release.load();});
            return jobHasFinished;
        }
    };
    audioalgo::editor::EditorWorkerPool pool(1);
    REQUIRE(pool.add_job(new Blocker(probe),true));
    QueueTarget target;
    auto dispatch=std::make_shared<audioalgo::editor::ComponentDispatchState<QueueTarget>>(target);
    audioalgo::editor::EditorRunQueue<QueueTarget> queue(pool,dispatch,nodes,std::make_shared<audioalgo::ProjectResourceService>(),
        [](QueueTarget& t,audioalgo::RunOutcome,audioalgo::editor::WaveformChannels,audioalgo::RunIdentity){++t.deliveries;});
    bool submitted=false,rejected_after_close=false,drained=false,timed_out=false,closing=false;
    struct Drive final:juce::Timer {std::function<void()> step;void timerCallback() override{step();}} drive;
    const auto started=std::chrono::steady_clock::now();auto closing_at=started;
    drive.step=[&]{
        if(probe->entered.load() && !closing) {
            audioalgo::RunRequest request;request.project.project_id="never-started";
            submitted=queue.submit(request,{"never-started",1,"pending",0});
            queue.close();rejected_after_close=!queue.submit(request,{"never-started",1,"rejected",0});
            closing=true;closing_at=std::chrono::steady_clock::now();
            (void)pool.shutdown_async(5000,[&](bool timeout){timed_out=timeout;drained=true;juce::MessageManager::getInstance()->stopDispatchLoop();});
        }
        if((closing && std::chrono::steady_clock::now()-closing_at>std::chrono::milliseconds(30)) ||
           std::chrono::steady_clock::now()-started>std::chrono::seconds(5)) {
            probe->release.store(true);probe->cv.notify_all();
        }
    };
    drive.startTimer(2);juce::MessageManager::getInstance()->runDispatchLoop();drive.stopTimer();pool.join_shutdown();
    CHECK(submitted);CHECK(rejected_after_close);CHECK(drained);CHECK_FALSE(timed_out);
    CHECK(pool.get_num_jobs()==0);CHECK(target.deliveries==0);
    const auto counts=queue.counts();CHECK(counts.active==0);CHECK(counts.pending==0);CHECK_FALSE(counts.worker_claimed);
}
