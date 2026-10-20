#pragma once

#include "audio_run_api/RunIdentity.h"

#include <optional>
#include <string>
#include <utility>

namespace audioalgo {

// Message-thread ownership. Every event must pass this boundary before any
// status, waveform, playback or current-result mutation in the view.
class RunReception {
public:
    void begin(RunIdentity identity) { active_ = std::move(identity); }
    void invalidate() noexcept { active_.reset(); }
    const std::optional<RunIdentity>& active() const noexcept { return active_; }

    bool accepts(const RunIdentity& event, const std::string& project_id,
                 std::uint64_t session_epoch, std::uint64_t revision) const noexcept {
        return active_.has_value() && event == *active_ && event.project_id == project_id &&
               event.session_epoch == session_epoch && event.revision == revision;
    }

private:
    std::optional<RunIdentity> active_;
};

} // namespace audioalgo
