#pragma once

#include "audio_core/Diagnostic.h"
#include "audio_juce_backend/JuceGraph.h"
#include "audio_project/ProjectFileIO.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <variant>

namespace audioalgo {

struct RunRequest {
    ProjectModel project;
    std::filesystem::path project_file;
    std::filesystem::path output_directory;
    int block_size = 512;
};

struct RunSuccess {
    std::string run_id;
    std::filesystem::path output_file;
    std::filesystem::path manifest_file;
    std::filesystem::path complete_file;
    std::size_t frames = 0;
    std::string input_sha256;
    std::string output_sha256;
};

using RunOutcome = std::variant<RunSuccess, Diagnostic>;

class RunService {
public:
    RunService(const NodeRegistry& descriptors, const JuceProcessorRegistry& processors);
    [[nodiscard]] RunOutcome run(const RunRequest& request) const;

private:
    const NodeRegistry& descriptors_;
    const JuceProcessorRegistry& processors_;
};

} // namespace audioalgo
