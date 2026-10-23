#pragma once

#include "audio_core/Diagnostic.h"
#include "audio_juce_backend/JuceGraph.h"
#include "audio_project/ProjectFileIO.h"
#include "audio_project/ProjectResourceService.h"
#include "audio_run_api/RunIdentity.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <variant>

namespace audioalgo {

struct RunRequest {
    ProjectModel project;
    std::filesystem::path project_file;
    std::filesystem::path output_directory;
    int block_size = 512;
    RunIdentity identity;
    // Worker-side, nonblocking predicate, valid through this synchronous run.
    // Once true it must remain true; the callback must not throw.
    std::function<bool()> is_cancelled;
};

struct RunSuccess {
    std::string run_id;
    std::filesystem::path output_file;
    std::filesystem::path manifest_file;
    std::filesystem::path complete_file;
    std::size_t frames = 0;
    std::string input_sha256;
    std::string output_sha256;
    RunIdentity identity;
};

using RunOutcome = std::variant<RunSuccess, Diagnostic>;

class RunService {
public:
    explicit RunService(const NodeRegistry& nodes,
                        std::shared_ptr<const ProjectResourceService> resources = {});
    explicit RunService(std::shared_ptr<const NodeRegistry> nodes,
                        std::shared_ptr<const ProjectResourceService> resources = {});
    [[nodiscard]] RunOutcome run(const RunRequest& request) const;

private:
    std::shared_ptr<const NodeRegistry> nodes_owner_;
    const NodeRegistry* nodes_ = nullptr;
    std::shared_ptr<const ProjectResourceService> resources_;
};

} // namespace audioalgo
