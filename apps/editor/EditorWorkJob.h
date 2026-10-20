#pragma once

#include "EditorCompletionDispatch.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <utility>

namespace audioalgo::editor {

// Runs value-only work on the editor pool and delivers its result only on the message thread.
template <typename ComponentType, typename ResultType>
class EditorWorkJob final : public juce::ThreadPoolJob {
public:
    using Work = std::function<ResultType()>;
    using Completion = std::function<void(ComponentType&, ResultType)>;

    EditorWorkJob(std::weak_ptr<ComponentDispatchState<ComponentType>> target,
                  Work work, Completion completion)
        : ThreadPoolJob("AudioAlgo editor file work"), target_(std::move(target)),
          work_(std::move(work)), completion_(std::move(completion)) {}

    JobStatus runJob() override {
        auto result = work_();
        const auto completion_scheduled = dispatch_to_live_component(
            target_, [result = std::move(result), completion = completion_](ComponentType& live_target) mutable {
                if (completion)
                    completion(live_target, std::move(result));
            });
        juce::ignoreUnused(completion_scheduled);
        return jobHasFinished;
    }

private:
    std::weak_ptr<ComponentDispatchState<ComponentType>> target_;
    Work work_;
    Completion completion_;
};

} // namespace audioalgo::editor
