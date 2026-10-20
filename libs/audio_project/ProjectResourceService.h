#pragma once

#include "audio_core/Diagnostic.h"

#include <filesystem>
#include <string>
#include <variant>

namespace audioalgo {

using ProjectResourcePathOutcome = std::variant<std::filesystem::path, Diagnostic>;
using ProjectResourceReferenceOutcome = std::variant<std::string, Diagnostic>;

// Resolves project-relative resources and converts chooser results into stable project references.
// This value-only service has no UI, JUCE, project-model, or audio-processing dependency.
class ProjectResourceService {
public:
    [[nodiscard]] ProjectResourcePathOutcome resolve(const std::string& reference,
                                                     const std::filesystem::path& project_file) const;
    [[nodiscard]] ProjectResourceReferenceOutcome make_reference(
        const std::filesystem::path& selected_resource, const std::filesystem::path& project_file) const;
    [[nodiscard]] ProjectResourceReferenceOutcome relocate(
        const std::string& missing_reference, const std::filesystem::path& project_file,
        const std::filesystem::path& selected_resource) const;
};

} // namespace audioalgo
