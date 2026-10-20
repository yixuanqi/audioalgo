#pragma once

#include "audio_node_sdk/NodeSDK.h"

namespace audioalgo {

// Shared composition entry used by the editor, CLI, and integration tests.
[[nodiscard]] bool register_builtin_nodes(NodeRegistry& registry);

} // namespace audioalgo
