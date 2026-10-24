#include "registration.h"
#include "plugins/builtin/registration.h"
#include "audio_node_authoring_runtime/PythonAuthoring.h"
#include <stdexcept>
#ifdef AUDIOALGO_HAS_SPEEXDSP
#include "plugins/speexdsp/DenoiseNode.h"
#endif
namespace audioalgo {
PluginDiscovery register_host_nodes(NodeRegistry& registry,const std::vector<std::filesystem::path>& roots) {
    if(!register_builtin_nodes(registry)) throw std::runtime_error("Builtin node registration failed");
#ifdef AUDIOALGO_HAS_SPEEXDSP
    if(!register_speex_nodes(registry)) throw std::runtime_error("Speex node registration failed");
#endif
    auto discovery = load_native_plugins(registry,roots);
    std::filesystem::path root;
    try {
    root = authoring::configured_authoring_root();
    auto restored = authoring::PythonAuthoring(root).restore_defaults(registry);
    if (const auto* error = std::get_if<authoring::SourceDiagnostic>(&restored))
        discovery.diagnostics.push_back({error->code, "", error->message, root / "index.json"});
    else {
        registry = std::get<NodeRegistry>(std::move(restored));
        for (const auto& [type, descriptor] : registry.descriptors())
            if (descriptor.plugin_origin && !descriptor.plugin_origin->managed_content_sha256.empty() &&
                std::find(discovery.loaded_plugin_ids.begin(), discovery.loaded_plugin_ids.end(), descriptor.plugin_origin->plugin_id) == discovery.loaded_plugin_ids.end())
                discovery.loaded_plugin_ids.push_back(descriptor.plugin_origin->plugin_id);
    }
    } catch (const std::exception& error) {
        discovery.diagnostics.push_back({"authoring_settings_invalid", "", error.what(), root});
    }
    return discovery;
}
}
