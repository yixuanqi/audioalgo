#pragma once
#include "audio_native_plugins/NativePlugins.h"
namespace audioalgo {
[[nodiscard]] PluginDiscovery register_host_nodes(NodeRegistry&, const std::vector<std::filesystem::path>& roots=configured_plugin_roots());
}
