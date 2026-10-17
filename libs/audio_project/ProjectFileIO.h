#pragma once

#include "audio_core/Diagnostic.h"
#include "audio_project/ProjectModel.h"

#include <filesystem>
#include <variant>

namespace audioalgo {

using ProjectLoadOutcome = std::variant<ProjectModel, Diagnostic>;
using ProjectSaveOutcome = std::variant<std::monostate, Diagnostic>;

[[nodiscard]] ProjectLoadOutcome load_project_file(const std::filesystem::path& path);
[[nodiscard]] ProjectSaveOutcome save_project_file(const std::filesystem::path& path, const ProjectModel& project);

} // namespace audioalgo
