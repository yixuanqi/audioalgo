#pragma once
#include "audio_node_sdk/NodeSDK.h"
#include <filesystem>

namespace audioalgo {
struct PluginDiagnostic {
    std::string code, plugin_id, message;
    std::filesystem::path manifest;
};
struct PluginDiscovery {
    std::vector<std::string> loaded_plugin_ids;
    std::vector<PluginDiagnostic> diagnostics;
};
// Explicit roots override defaults. Otherwise AUDIOALGO_PLUGIN_PATH (semicolon separated)
// overrides <exe>/plugins and %APPDATA%/AudioAlgo/plugins. Scan root and one package level.
[[nodiscard]] std::vector<std::filesystem::path> configured_plugin_roots();
// Deterministic order; every package commits descriptors and factories atomically.
// Registry factories and instances own module leases; no explicit unload API exists.
[[nodiscard]] PluginDiscovery load_native_plugins(NodeRegistry&, const std::vector<std::filesystem::path>&);
[[nodiscard]] std::string node_catalog_json(const NodeRegistry&, const PluginDiscovery&);
}
