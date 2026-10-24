#pragma once

#include "audio_core/Diagnostic.h"
#include "audio_project/ProjectModel.h"

#include <filesystem>
#include <functional>
#include <variant>

namespace audioalgo {

using ProjectLoadOutcome = std::variant<ProjectModel, Diagnostic>;
using ProjectSaveOutcome = std::variant<std::monostate, Diagnostic>;
enum class ProjectSavePhase { backup, write, verify, replace };
struct ProjectSaveOptions {
    std::function<bool(ProjectSavePhase)> allow;
};

[[nodiscard]] ProjectLoadOutcome load_project_file(const std::filesystem::path& path);
[[nodiscard]] ProjectSaveOutcome save_project_file(const std::filesystem::path& path, const ProjectModel& project);
[[nodiscard]] ProjectSaveOutcome save_project_file(const std::filesystem::path& path, const ProjectModel& project,
                                                  const ProjectSaveOptions& options);

} // namespace audioalgo
