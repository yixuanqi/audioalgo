#include "registration.h"
#include "plugins/builtin/registration.h"
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
    return load_native_plugins(registry,roots);
}
}
