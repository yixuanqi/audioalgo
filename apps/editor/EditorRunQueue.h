#pragma once
#include "EditorRunJob.h"
#include "EditorWorkerPool.h"
#include "audio_editor_application/LatestRequestMailbox.h"

namespace audioalgo::editor {
// The production run submission boundary. File/document work remains on the
// existing pool; render requests have one place to implement replacement policy.
template<typename ComponentType>
class EditorRunQueue final {
public:
    using Completion=typename EditorRunJob<ComponentType>::Completion;
    using LogCompletion=typename EditorRunJob<ComponentType>::LogCompletion;
    struct Request {RunRequest run;RunIdentity identity;std::shared_ptr<const NodeRegistry> nodes;};
    using Mailbox=LatestRequestMailbox<Request>;
    EditorRunQueue(EditorWorkerPool& pool,std::weak_ptr<ComponentDispatchState<ComponentType>> target,
                   std::shared_ptr<const NodeRegistry> nodes,std::shared_ptr<const ProjectResourceService> resources,
                   Completion completion,LogCompletion logs={})
        :pool_(pool),state_(std::make_shared<State>(std::move(target),std::move(nodes),std::move(resources),std::move(completion),std::move(logs))){}
    ~EditorRunQueue(){close();}
    EditorRunQueue(const EditorRunQueue&)=delete;
    EditorRunQueue& operator=(const EditorRunQueue&)=delete;
    [[nodiscard]] bool submit(RunRequest request,RunIdentity identity) {
        // Capture on the submitting message thread. Workers never read a map
        // being replaced by a catalog refresh, including requests still pending.
        auto snapshot = std::make_shared<const NodeRegistry>(*state_->nodes);
        const auto submission=state_->mailbox.submit({std::move(request),std::move(identity),std::move(snapshot)});
        if(!submission.accepted)return false;
        if(!submission.start_worker)return true;
        auto* job=new DrainJob(state_,submission.ticket);
        if(pool_.add_job(job,true))return true;
        delete job;state_->mailbox.abandon(submission.ticket);return false;
    }
    void cancel(){state_->mailbox.cancel();}
    void close(){state_->mailbox.cancel(true);}
    [[nodiscard]] typename Mailbox::Counts counts() const{return state_->mailbox.counts();}
private:
    struct State {
        State(std::weak_ptr<ComponentDispatchState<ComponentType>> t,std::shared_ptr<const NodeRegistry> n,
              std::shared_ptr<const ProjectResourceService> r,Completion c,LogCompletion l)
            :target(std::move(t)),nodes(std::move(n)),resources(std::move(r)),completion(std::move(c)),logs(std::move(l)){}
        Mailbox mailbox;
        std::weak_ptr<ComponentDispatchState<ComponentType>> target;
        std::shared_ptr<const NodeRegistry> nodes;
        std::shared_ptr<const ProjectResourceService> resources;
        Completion completion;
        LogCompletion logs;
    };
    class DrainJob final:public juce::ThreadPoolJob {
    public:
        DrainJob(std::shared_ptr<State> state,typename Mailbox::Ticket ticket)
            :ThreadPoolJob("AudioAlgo latest renders"),state_(std::move(state)),ticket_(ticket){}
        ~DrainJob() override {state_->mailbox.abandon(ticket_);}
        JobStatus runJob() override {
            while(!shouldExit()) {
                auto entry=state_->mailbox.take(ticket_);
                if(!entry)return jobHasFinished;
                auto& request=entry->request;
                auto original=std::move(request.run.is_cancelled);
                request.run.is_cancelled=[this,token=entry->cancelled,original=std::move(original)] {
                    return shouldExit()||token->load()||(original && original());
                };
                try {
                    // Reuse the actual worker's service, waveform and identity delivery
                    // path. The inner job is synchronous inside this pool worker.
                    EditorRunJob<ComponentType> one(state_->target,request.nodes,std::move(request.run),
                        request.identity,state_->resources,state_->completion,state_->logs);
                    one.runJob();
                } catch(const std::exception& error) {
                    const auto posted=dispatch_to_live_component(state_->target,
                        [completion=state_->completion,identity=request.identity,message=std::string(error.what())](ComponentType& target) mutable {
                            completion(target,Diagnostic{"run_exception",std::move(message)},{},std::move(identity));
                        });
                    juce::ignoreUnused(posted);
                }
            }
            state_->mailbox.abandon(ticket_);return jobHasFinished;
        }
    private:
        std::shared_ptr<State> state_;
        typename Mailbox::Ticket ticket_;
    };
    EditorWorkerPool& pool_;
    std::shared_ptr<State> state_;
};
}
