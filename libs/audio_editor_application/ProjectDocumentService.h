#pragma once

#include "audio_core/Diagnostic.h"
#include "audio_project/ProjectModel.h"

#include <filesystem>
#include <variant>

namespace audioalgo {

using ProjectDocumentLoadOutcome = std::variant<ProjectModel, Diagnostic>;
using ProjectDocumentSaveOutcome = std::variant<std::monostate, Diagnostic>;

// Host-provided project persistence. Implementations may use files, cloud storage, or tests;
// the application layer only sees value models and diagnostics.
class ProjectDocumentService {
public:
    virtual ~ProjectDocumentService() = default;
    [[nodiscard]] virtual ProjectDocumentLoadOutcome load(const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual ProjectDocumentSaveOutcome save(const std::filesystem::path& path,
                                                          const ProjectModel& project) const = 0;
};

} // namespace audioalgo
