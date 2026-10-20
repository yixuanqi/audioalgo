#pragma once

#include <cstdint>
#include <string>

namespace audioalgo {

// Allocated before enqueueing. Revision alone is not an execution identity.
struct RunIdentity {
    std::string project_id;
    std::uint64_t session_epoch = 0;
    std::string request_id;
    std::uint64_t revision = 0;

    bool operator==(const RunIdentity&) const = default;
};

} // namespace audioalgo
