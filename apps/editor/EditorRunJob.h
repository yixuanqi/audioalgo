#pragma once

#include "EditorCompletionDispatch.h"
#include "EditorWaveformData.h"
#include "audio_io/WavFile.h"
#include "audio_run_service/RunService.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace audioalgo::editor {

template <typename ComponentType>
class EditorRunJob final : public juce::ThreadPoolJob {
public:
    using Completion = std::function<void(ComponentType&, RunOutcome, WaveformChannels, RunIdentity)>;

    EditorRunJob(std::weak_ptr<ComponentDispatchState<ComponentType>> target,
                 std::shared_ptr<const NodeRegistry> descriptors, RunRequest request,
                 RunIdentity identity, Completion completion)
        : EditorRunJob(std::move(target), std::move(descriptors), std::move(request), std::move(identity),
                       std::make_shared<ProjectResourceService>(), std::move(completion)) {}

    EditorRunJob(std::weak_ptr<ComponentDispatchState<ComponentType>> target,
                 std::shared_ptr<const NodeRegistry> descriptors, RunRequest request,
                 RunIdentity identity, std::shared_ptr<const ProjectResourceService> resources,
                 Completion completion)
        : ThreadPoolJob("AudioAlgo offline render"), target_(std::move(target)),
          descriptors_(std::move(descriptors)), request_(std::move(request)),
          identity_(std::move(identity)), resources_(resources ? std::move(resources)
                                                               : std::make_shared<ProjectResourceService>()),
          completion_(std::move(completion)) {}

    JobStatus runJob() override {
        std::error_code filesystem_error;
        std::filesystem::create_directories(request_.output_directory.parent_path(), filesystem_error);
        RunService service(descriptors_, resources_);
        auto outcome = filesystem_error
            ? RunOutcome(Diagnostic{"run_directory_create_failed", filesystem_error.message()})
            : service.run(request_);

        WaveformChannels waveform;
        if (const auto* success = std::get_if<RunSuccess>(&outcome)) {
            const auto audio = read_wav_file(success->output_file);
            if (const auto* buffer = std::get_if<AudioBuffer>(&audio))
                waveform = make_waveform(*buffer);
        }

        const auto completion_scheduled = dispatch_to_live_component(
            target_,
            [outcome = std::move(outcome), waveform = std::move(waveform), identity = identity_,
             completion = completion_](ComponentType& live_target) mutable {
                if (completion)
                    completion(live_target, std::move(outcome), std::move(waveform), identity);
            });
        juce::ignoreUnused(completion_scheduled);
        return jobHasFinished;
    }

private:
    std::weak_ptr<ComponentDispatchState<ComponentType>> target_;
    std::shared_ptr<const NodeRegistry> descriptors_;
    RunRequest request_;
    RunIdentity identity_;
    std::shared_ptr<const ProjectResourceService> resources_;
    Completion completion_;
};

} // namespace audioalgo::editor
