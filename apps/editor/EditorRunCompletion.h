#pragma once

#include "audio_editor_application/RunReception.h"

#include <cstdint>
#include <string>
#include <utility>

namespace audioalgo::editor {

// Keep production View updates behind the same identity gate used by
// RunReception. A completion body must not inspect or update the View until
// the project, session, request, and revision still match.
template <typename Completion>
[[nodiscard]] bool apply_if_current_run(const RunReception& reception, const RunIdentity& identity,
                                        const std::string& current_project_id,
                                        std::uint64_t current_session_epoch,
                                        std::uint64_t current_revision,
                                        Completion&& completion) {
    if (!reception.accepts(identity, current_project_id, current_session_epoch, current_revision))
        return false;

    std::forward<Completion>(completion)();
    return true;
}

} // namespace audioalgo::editor
