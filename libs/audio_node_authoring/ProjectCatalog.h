#pragma once
#include "AuthoringChecks.h"
#include "audio_project/ProjectModel.h"

namespace audioalgo::authoring {
// Value adapter used by host composition roots; the authoring service does not own a project.
inline std::vector<PackageSelection> managed_selections(const ProjectModel& project) {
    std::vector<PackageSelection> result;
    for (const auto& node : project.nodes) if (node.managed_version) {
        const auto& v = *node.managed_version;
        result.push_back({node.id, node.type_id, v.package_id, v.content_sha256, v.package_path, v.wrapper_version});
    }
    return result;
}
}
