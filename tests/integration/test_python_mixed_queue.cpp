#include "../fixtures/PythonMixedFault.h"
#include "EditorRunQueue.h"
#include "audio_editor_application/EditorController.h"
#include "audio_editor_application/RunReception.h"
#include <atomic>
#include <thread>
#include <cmath>

namespace {
struct QueueView final:juce::Component {
    explicit QueueView(const audioalgo::NodeCatalog& nodes):controller(nodes){}
    audioalgo::editor::EditorController controller;audioalgo::RunReception reception;
    std::uint64_t epoch=0;int delivered=0,stale=0;std::optional<audioalgo::RunOutcome> result;
};
struct Tick final:juce::Timer {std::function<void()> step;void timerCallback() override{step();}};
bool entered(const std::filesystem::path& root) {
    std::error_code error;
    for(std::filesystem::directory_iterator it(root,error),end;!error && it!=end;it.increment(error))
        if(it->path().filename().string().starts_with("python-task-") && std::filesystem::exists(it->path()/"entered.txt"))return true;
    return false;
}
void check_samples(const audioalgo::RunSuccess& success,const audioalgo::AudioBuffer& input,double scale) {
    const auto output=std::get<audioalgo::AudioBuffer>(audioalgo::read_wav_file(success.output_file));REQUIRE(output.frame_count()==1025);REQUIRE(output.channel_count()==2);
    for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1025;++i){const float expected=static_cast<float>(static_cast<float>(input.channel(c)[i]*2)*static_cast<float>(scale)+static_cast<float>(static_cast<float>(i)*.00001f))*.5f;CHECK(std::abs(output.channel(c)[i]-expected)<=2e-6+1e-5*std::abs(expected));}
}
}

TEST_CASE("P3-05 production mixed queue retains only active and latest of fifty submissions","[P3-05][queue]") {
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI messages;test::PythonMixedFault fixture;QueueView view(*fixture.registry);view.controller.adopt_project(fixture.project);view.epoch=1;
    auto dispatch=std::make_shared<editor::ComponentDispatchState<QueueView>>(view);editor::EditorWorkerPool pool(1);
    editor::EditorRunQueue<QueueView> queue(pool,dispatch,fixture.registry,std::make_shared<ProjectResourceService>(),
        [](QueueView& v,RunOutcome result,editor::WaveformChannels,RunIdentity id){
            if(v.reception.accepts(id,v.controller.project().project_id,v.epoch,v.controller.revision())){++v.delivered;v.result=std::move(result);}else ++v.stale;
        });
    const auto submit=[&](int i){auto r=fixture.request("burst-"+std::to_string(i),i==0?"wait":"ok");r.project.revision=i+1;
        view.controller.adopt_project(r.project);RunIdentity id{r.project.project_id,1,"burst-"+std::to_string(i),r.project.revision};view.reception.begin(id);r.identity=id;return queue.submit(std::move(r),id);};
    REQUIRE(submit(0));std::atomic<bool> done=false,ready=false;std::thread watcher([&]{while(!done.load() && !ready.load()){ready=entered(fixture.root/"burst-0");std::this_thread::sleep_for(std::chrono::milliseconds(5));}});
    bool sent=false,timed_out=false,closing=false,accepted=true;std::size_t peak_active=0,peak_pending=0;Tick drive;const auto started=std::chrono::steady_clock::now();
    drive.step=[&]{if(ready.load() && !sent){for(int i=1;i<=50;++i)accepted=submit(i)&&accepted;sent=true;}
        const auto count=queue.counts();peak_active=std::max(peak_active,count.active);peak_pending=std::max(peak_pending,count.pending);
        if(std::chrono::steady_clock::now()-started>std::chrono::seconds(10))timed_out=true;
        if(!closing && ((view.result && pool.get_num_jobs()==0)||timed_out)){closing=true;drive.stopTimer();queue.close();(void)pool.shutdown_async(5000,[](bool){juce::MessageManager::getInstance()->stopDispatchLoop();});}
    };
    drive.startTimer(2);juce::MessageManager::getInstance()->runDispatchLoop();done=true;watcher.join();pool.join_shutdown();
    REQUIRE_FALSE(timed_out);CHECK(sent);CHECK(accepted);CHECK(peak_active<=1);CHECK(peak_pending<=1);CHECK(view.stale==1);CHECK(view.delivered==1);
    REQUIRE(view.result);REQUIRE(std::holds_alternative<RunSuccess>(*view.result));const auto success=std::get<RunSuccess>(*view.result);CHECK(success.identity.request_id=="burst-50");
    check_samples(success,std::get<AudioBuffer>(read_wav_file(fixture.root/"input.wav")),.75);
    for(int i=0;i<50;++i)CHECK_FALSE(std::filesystem::exists(fixture.root/("burst-"+std::to_string(i))));
}

TEST_CASE("P3-05 fifty mixed edit run cancel reopen and close cycles keep workers bounded and UI heartbeat responsive","[P3-05][cycles]") {
    using namespace audioalgo;juce::ScopedJuceInitialiser_GUI messages;test::PythonMixedFault fixture;editor::EditorWorkerPool pool(1);
    using Dispatch=editor::ComponentDispatchState<QueueView>;using Queue=editor::EditorRunQueue<QueueView>;
    std::unique_ptr<QueueView> view;std::shared_ptr<Dispatch> dispatch;std::unique_ptr<Queue> queue;
    std::vector<std::weak_ptr<Dispatch>> retired;std::vector<std::pair<RunSuccess,double>> successes;
    int completed=0,stale=0,closed=0,cycle=0,accepted_errors=0;bool submitted=false,cancelled=false,closing_view=false,finished=false,timed_out=false,shutdown_timeout=false,retrying=false;
    std::size_t peak_active=0,peak_pending=0;std::atomic<int> watched=-1,observed=-1;std::atomic<bool> stop=false;
    std::thread watcher([&]{while(!stop.load()){const auto n=watched.load();if(n>=0 && observed.load()!=n && entered(fixture.root/("cycle-"+std::to_string(n))))observed=n;std::this_thread::sleep_for(std::chrono::milliseconds(5));}});
    const auto create_view=[&]{view=std::make_unique<QueueView>(*fixture.registry);dispatch=std::make_shared<Dispatch>(*view);
        queue=std::make_unique<Queue>(pool,dispatch,fixture.registry,std::make_shared<ProjectResourceService>(),
            [&](QueueView& v,RunOutcome result,editor::WaveformChannels,RunIdentity id){++completed;
                if(v.reception.accepts(id,v.controller.project().project_id,v.epoch,v.controller.revision())){++v.delivered;v.result=std::move(result);}else {++v.stale;++stale;}
            });
    };create_view();
    Tick drive;std::vector<double> gaps;auto previous=std::chrono::steady_clock::now();const auto started=previous;int completion_before=0;
    const auto is_close=[](int n){return n%10==9;};const auto is_wait=[&](int n){return n%3==0 || is_close(n);};
    drive.step=[&]{const auto now=std::chrono::steady_clock::now();gaps.push_back(std::chrono::duration<double,std::milli>(now-previous).count());previous=now;
        if(now-started>std::chrono::seconds(50))timed_out=true;
        if(!submitted && cycle<50 && !timed_out) {
            if(!retrying){view->controller.adopt_project(fixture.project);view->epoch=cycle+1;}
            view->result.reset();const bool crash=cycle==25 && !retrying;
            (void)view->controller.set_parameter("python","scale",.25+cycle*.005);(void)view->controller.set_parameter("python","mode",std::string(crash?"crash":is_wait(cycle)?"wait":"ok"));
            const auto name="cycle-"+std::to_string(cycle)+(crash?"-crash":"");auto r=fixture.request(name,"ok");r.project=view->controller.project();RunIdentity id{r.project.project_id,view->epoch,name,r.project.revision};r.identity=id;view->reception.begin(id);
            completion_before=completed;submitted=queue->submit(std::move(r),id);watched=is_wait(cycle)?cycle:-1;
        }
        if(submitted && is_wait(cycle) && observed.load()==cycle && !cancelled) {
            cancelled=true;watched=-1;
            if(is_close(cycle)){queue->close();dispatch->invalidate();retired.push_back(dispatch);queue.reset();dispatch.reset();view.reset();closing_view=true;++closed;}
            else {queue->cancel();view->controller.adopt_project(fixture.project);++view->epoch;view->reception.invalidate();view->result.reset();}
        }
        if(queue){const auto count=queue->counts();peak_active=std::max(peak_active,count.active);peak_pending=std::max(peak_pending,count.pending);}
        if(submitted && pool.get_num_jobs()==0 && (closing_view || completed>completion_before)) {
            if(cycle==25 && !retrying && view && view->result && std::holds_alternative<Diagnostic>(*view->result)) {
                ++accepted_errors;retrying=true;submitted=false; // Same open project, a fresh valid request follows the crash.
            } else {
                if(!is_wait(cycle)){if(view && view->result && std::holds_alternative<RunSuccess>(*view->result))successes.emplace_back(std::get<RunSuccess>(*view->result),.25+cycle*.005);}
                ++cycle;submitted=false;cancelled=false;retrying=false;if(closing_view){create_view();closing_view=false;}
            }
        }
        if(!finished && (cycle==50 || timed_out)){finished=true;drive.stopTimer();watched=-1;if(queue)queue->close();(void)pool.shutdown_async(5000,[&](bool timeout){shutdown_timeout=timeout;juce::MessageManager::getInstance()->stopDispatchLoop();});}
    };
    drive.startTimer(2);juce::MessageManager::getInstance()->runDispatchLoop();drive.stopTimer();stop=true;watcher.join();pool.join_shutdown();
    REQUIRE_FALSE(timed_out);CHECK_FALSE(shutdown_timeout);CHECK(cycle==50);CHECK(closed==5);CHECK(stale==15);CHECK(successes.size()==30);CHECK(accepted_errors==1);CHECK(pool.get_num_jobs()==0);CHECK(peak_active<=1);CHECK(peak_pending<=1);
    if(queue){const auto count=queue->counts();CHECK(count.active==0);CHECK(count.pending==0);CHECK_FALSE(count.worker_claimed);}
    queue.reset();if(dispatch)dispatch->invalidate();dispatch.reset();view.reset();for(const auto& old:retired)CHECK(old.expired());CHECK(fixture.registry.use_count()==1);
    REQUIRE(gaps.size()>100);std::sort(gaps.begin(),gaps.end());const auto p95=gaps[static_cast<std::size_t>(std::ceil(gaps.size()*.95))-1];CHECK(p95<=100);CHECK(gaps.back()<=500);
    const auto input=std::get<AudioBuffer>(read_wav_file(fixture.root/"input.wav"));for(const auto& [success,scale]:successes)check_samples(success,input,scale);
    for(int i=0;i<50;++i)CHECK(std::filesystem::exists(fixture.root/("cycle-"+std::to_string(i))/"complete.json")==!is_wait(i));
    CHECK_FALSE(std::filesystem::exists(fixture.root/"cycle-25-crash"));
    juce::DynamicObject::Ptr evidence=new juce::DynamicObject;evidence->setProperty("cycles",cycle);evidence->setProperty("successes",static_cast<int>(successes.size()));evidence->setProperty("stale_rejected",stale);evidence->setProperty("views_closed",closed);
    evidence->setProperty("heartbeat_p95_ms",p95);evidence->setProperty("heartbeat_max_ms",gaps.back());evidence->setProperty("peak_active",static_cast<int>(peak_active));evidence->setProperty("peak_pending",static_cast<int>(peak_pending));
    evidence->setProperty("accepted_errors",accepted_errors);
    REQUIRE(juce::File((fixture.root/"queue-evidence.json").string()).replaceWithText(juce::JSON::toString(juce::var(evidence.get()))));
}
