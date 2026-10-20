#pragma once

#include <juce_events/juce_events.h>

#include <functional>
#include <thread>
#include <utility>

namespace audioalgo::editor {

class EditorWorkerPool final {
public:
    using ShutdownCompletion = std::function<void(bool first_wait_timed_out)>;

    explicit EditorWorkerPool(int thread_count) : pool_(thread_count) {}

    ~EditorWorkerPool() {
        join_shutdown();
        jassert(pool_.getNumJobs() == 0);
    }

    bool add_job(juce::ThreadPoolJob* job, bool delete_when_finished) {
        if (shutdown_started_ || job == nullptr)
            return false;
        pool_.addJob(job, delete_when_finished);
        return true;
    }

    [[nodiscard]] bool shutdown_async(int first_wait_timeout_ms, ShutdownCompletion completion) {
        jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
        if (shutdown_started_)
            return false;

        shutdown_started_ = true;
        shutdown_thread_ = std::thread(
            [this, first_wait_timeout_ms, completion = std::move(completion)]() mutable {
                const bool first_wait_timed_out = !pool_.removeAllJobs(true, first_wait_timeout_ms);
                if (first_wait_timed_out) {
                    const bool drained = pool_.removeAllJobs(true, -1);
                    jassert(drained);
                }

                const bool callback_posted = juce::MessageManager::callAsync(
                    [completion = std::move(completion), first_wait_timed_out]() mutable {
                        if (completion)
                            completion(first_wait_timed_out);
                    });
                jassert(callback_posted);
            });
        return true;
    }

    void join_shutdown() {
        if (shutdown_thread_.joinable())
            shutdown_thread_.join();
    }

    [[nodiscard]] int get_num_jobs() const noexcept { return pool_.getNumJobs(); }

private:
    juce::ThreadPool pool_;
    std::thread shutdown_thread_;
    bool shutdown_started_ = false;
};

} // namespace audioalgo::editor
