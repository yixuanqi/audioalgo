#pragma once
#include "RunIdentity.h"
#include <cstdint>
#include <string>

namespace audioalgo {
// Owned worker-to-observer value. No borrowed buffers or GUI dependencies.
struct NodeLogEvent {
    RunIdentity identity;
    std::string node_id, type_id, version, package_id, content_sha256;
    std::uint64_t sequence = 0, received_unix_ms = 0;
    std::string source, level, text, report_file, raw_log_file;
    std::uint64_t total_bytes = 0, discarded_bytes = 0;
    std::uint64_t dropped_text_bytes = 0;
    std::uint64_t ui_dropped_text_bytes = 0, ui_dropped_events = 0;
};
}
