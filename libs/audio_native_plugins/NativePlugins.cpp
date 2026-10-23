#include "NativePlugins.h"
#include <audioalgo/node_abi_v1.h>
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace audioalgo {
namespace {
juce::String js(const std::string& s) { return juce::String::fromUTF8(s.data(), static_cast<int>(s.size())); }
std::string text(const juce::var& v) { return v.toString().toStdString(); }
juce::File file(const std::filesystem::path& p) {
    auto s=p.u8string();
    return juce::File(juce::String::fromUTF8(reinterpret_cast<const char*>(s.data()),static_cast<int>(s.size())));
}
struct Rejected : std::runtime_error {
    std::string code;
    Rejected(std::string c, std::string m):std::runtime_error(std::move(m)),code(std::move(c)){}
};
void require(bool ok, const char* message) { if(!ok) throw Rejected("plugin_manifest_invalid",message); }
std::string string_field(const juce::var& v, const char* k) {
    auto value=v.getProperty(k,{});
    require(value.isString() && value.toString().isNotEmpty(), k);
    return text(value);
}
int positive(const juce::var& v) {
    require((v.isInt()||v.isInt64()) && static_cast<juce::int64>(v)>0 &&
            static_cast<juce::int64>(v)<=std::numeric_limits<int>::max(),"Expected positive integer");
    return static_cast<int>(v);
}
bool boolean_field(const juce::var& v,const char* k,bool fallback) {
    if(!v.hasProperty(k)) return fallback;
    require(v[k].isBool(), k); return static_cast<bool>(v[k]);
}
const juce::Array<juce::var>& array_field(const juce::var& v,const char* k) {
    auto* p=v.getDynamicObject(); require(p!=nullptr,"Expected object");
    auto* a=p->getProperty(k).getArray(); require(a!=nullptr,k); return *a;
}
ParameterValue parameter_value(const juce::var& v, ParameterKind kind) {
    if(kind==ParameterKind::boolean) { require(v.isBool(),"Boolean default required"); return static_cast<bool>(v); }
    if(kind==ParameterKind::integer) { require(v.isInt()||v.isInt64(),"Integer default required"); return static_cast<std::int64_t>(static_cast<juce::int64>(v)); }
    if(kind==ParameterKind::floating) { require(v.isDouble()||v.isInt()||v.isInt64(),"Number default required"); return static_cast<double>(v); }
    require(v.isString(),"String default required"); return text(v);
}
NodeDescriptor descriptor(const juce::var& v) {
    NodeDescriptor d;
    d.type_id=string_field(v,"type_id"); d.version=string_field(v,"version"); d.display_name=string_field(v,"display_name");
    if(v.hasProperty("execution_kind") && text(v["execution_kind"])!="native_block")
        throw Rejected("plugin_capability_unsupported","ABI v1 supports native_block PCM transforms only");
    for(auto& p:array_field(v,"ports")) {
        auto direction=string_field(p,"direction");
        require(direction=="input"||direction=="output","Invalid port direction");
        auto type=string_field(p,"type_id");
        if(type!="audio.pcm@1") throw Rejected("plugin_capability_unsupported","ABI v1 requires audio.pcm@1 ports");
        d.ports.push_back({string_field(p,"id"),type,direction=="input"?PortDirection::input:PortDirection::output,boolean_field(p,"required",false)});
    }
    for(auto& p:array_field(v,"parameters")) {
        const auto kind=string_field(p,"kind");
        static const std::map<std::string,ParameterKind> kinds={{"bool",ParameterKind::boolean},{"int",ParameterKind::integer},{"float",ParameterKind::floating},{"enum",ParameterKind::enumeration},{"string",ParameterKind::string},{"path",ParameterKind::path}};
        require(kinds.contains(kind),"Unsupported parameter kind");
        ParameterDescriptor param{string_field(p,"id"),kinds.at(kind),parameter_value(p["default"],kinds.at(kind))};
        for(auto [key,target]:{std::pair{"minimum",&param.minimum},{"maximum",&param.maximum},{"step",&param.step}}) {
            if(p.hasProperty(key)) { require(p[key].isDouble()||p[key].isInt()||p[key].isInt64(),key); *target=static_cast<double>(p[key]); }
        }
        param.unit=text(p["unit"]); param.display_name=text(p["display_name"]); param.help=text(p["help"]);
        param.input_resource=boolean_field(p,"input_resource",false);
        if(p.hasProperty("choices")) for(auto& c:array_field(p,"choices")) param.choices.push_back({string_field(c,"value"),string_field(c,"label")});
        d.parameters.push_back(std::move(param));
    }
    for(auto& rate:array_field(v,"sample_rates_hz")) d.audio_format.sample_rates_hz.push_back(positive(rate));
    for(auto& channels:array_field(v,"channel_counts")) d.audio_format.channel_counts.push_back(positive(channels));
    d.capabilities.supports_named_audio_ports=true;
    if(boolean_field(v,"uses_streaming_frame_api",false))
        throw Rejected("plugin_capability_unsupported","Published C ABI v1 supports full-frame processors only");
    d.capabilities.supports_variable_block_size=boolean_field(v,"supports_variable_block_size",true);
    if(v.hasProperty("fixed_block_frames")) d.capabilities.fixed_block_frames=positive(v["fixed_block_frames"]);
    if(v.hasProperty("fixed_frame_end_policy")) {
        const auto policy=string_field(v,"fixed_frame_end_policy");
        require(policy=="zero_pad" || policy=="reject_partial","Unsupported fixed frame EOS policy");
        if(policy=="zero_pad") d.capabilities.fixed_frame_end_policy=NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
    }
    // Nonzero latency/tail needs a later ABI; never accept and then drop that declaration.
    for(auto* key:{"latency_frames","tail_frames"}) if(v.hasProperty(key)) {
        require((v[key].isInt()||v[key].isInt64()) && static_cast<juce::int64>(v[key])>=0,key);
        if(std::string_view(key)=="latency_frames") d.capabilities.latency_frames=static_cast<std::size_t>(static_cast<juce::int64>(v[key]));
        else if(static_cast<juce::int64>(v[key])!=0)
            throw Rejected("plugin_capability_unsupported","ABI v1 has no algorithm tail/drain contract");
    }
    const auto inputs=std::count_if(d.ports.begin(),d.ports.end(),[](auto& p){return p.direction==PortDirection::input;});
    require(inputs>0 && static_cast<std::size_t>(inputs)<d.ports.size(),"Transform needs inputs and outputs");
    return d;
}
aa_string borrowed(const std::string_view s) { return {s.data(),static_cast<uint32_t>(s.size())}; }
template<std::size_t N> std::string bounded(const char (&s)[N]) { return {s,std::find(s,s+N,'\0')}; }
Diagnostic failure(const aa_error& e,const char* fallback) {
    return {e.code[0]?bounded(e.code):fallback,bounded(e.message)};
}
struct Module {
#ifdef _WIN32
    HMODULE handle=nullptr;
    ~Module(){ if(handle) FreeLibrary(handle); }
#else
    ~Module()=default;
#endif
    aa_node_api_v1 api{};
};
class Processor final : public AudioNodeProcessor {
public:
    Processor(std::shared_ptr<Module> module,NodeDescriptor desc):module_(std::move(module)),desc_(std::move(desc)) {
        aa_error e{sizeof(e)};
        if(module_->api.create(borrowed(desc_.type_id),&instance_,&e)!=0 || !instance_) {
            // A failed create that returns partial storage must still be destroyed in its DLL.
            if(instance_) { module_->api.destroy(instance_); instance_=nullptr; }
            throw std::runtime_error(failure(e,"plugin_create_failed").message);
        }
        for(auto& p:desc_.ports) if(p.direction==PortDirection::input) inputs_.push_back({borrowed(p.id),0,nullptr});
                               else outputs_.push_back({borrowed(p.id),0,nullptr});
    }
    ~Processor() override { if(instance_) { module_->api.reset(instance_); module_->api.destroy(instance_); } }
    NodeProcessOutcome process(const NodeProcessRequest&) override { return Diagnostic{"plugin_owned_processing_unsupported","Use the native block runtime"}; }
    bool supports_block_processing() const noexcept override {return true;}
    bool supports_named_block_processing() const noexcept override {return true;}
    std::optional<Diagnostic> prepare(const NodePrepareContext& c) override {
        if(c.maximum_block_frames>UINT32_MAX || c.format.channel_count>UINT32_MAX) return Diagnostic{"plugin_format_overflow","ABI frame/channel limit exceeded"};
        juce::DynamicObject::Ptr parameters=new juce::DynamicObject;
        for(auto& p:desc_.parameters) {
            auto value=p.default_value;
            if(c.parameters) if(auto it=c.parameters->find(p.id);it!=c.parameters->end()) value=it->second;
            if(auto error=validate_parameter_value(p,value)) return Diagnostic{error->code,error->message};
            std::visit([&](const auto& x){
                using T=std::decay_t<decltype(x)>;
                if constexpr(std::is_same_v<T,std::string>) parameters->setProperty(js(p.id),js(x));
                else if constexpr(std::is_same_v<T,std::int64_t>) parameters->setProperty(js(p.id),static_cast<juce::int64>(x));
                else parameters->setProperty(js(p.id),x);
            },value);
        }
        const auto json=juce::JSON::toString(juce::var(parameters.get()),true).toStdString();
        aa_prepare_v1 context{sizeof(context),c.format.sample_rate_hz,static_cast<uint32_t>(c.format.channel_count),static_cast<uint32_t>(c.maximum_block_frames),borrowed(json)};
        aa_error e{sizeof(e)};
        if(module_->api.prepare(instance_,&context,&e)!=0) return failure(e,"plugin_prepare_failed");
        maximum_=context.maximum_block_frames; channels_=context.channels; rate_=context.sample_rate_hz;
        return {};
    }
    AudioBlockOutcome process_named_block(const NamedAudioBlockView& b) override {
        if(b.inputs.size()!=inputs_.size() || b.outputs.size()!=outputs_.size() || b.frame_count>maximum_ || b.sample_rate_hz!=rate_)
            return Diagnostic{"plugin_block_shape","Invalid named PCM block shape"};
        for(std::size_t i=0;i<inputs_.size();++i) {
            if(b.inputs[i].channels.size()!=channels_) return Diagnostic{"plugin_block_channels","Input channel count changed"};
            inputs_[i]={borrowed(b.inputs[i].id),channels_,b.inputs[i].channels.data()};
        }
        for(std::size_t i=0;i<outputs_.size();++i) {
            if(b.outputs[i].channels.size()!=channels_) return Diagnostic{"plugin_block_channels","Output channel count changed"};
            outputs_[i]={borrowed(b.outputs[i].id),channels_,b.outputs[i].channels.data()};
        }
        aa_block_v1 block{sizeof(block),b.sample_rate_hz,static_cast<uint32_t>(b.frame_count),static_cast<uint32_t>(inputs_.size()),static_cast<uint32_t>(outputs_.size()),inputs_.data(),outputs_.data()};
        aa_result_v1 result{sizeof(result),0,0}; aa_error e{sizeof(e)};
        if(module_->api.process(instance_,&block,&result,&e)!=0) return failure(e,"plugin_process_failed");
        if(result.struct_size!=sizeof(result) || result.consumed_frames!=block.frames || result.produced_frames!=block.frames)
            return Diagnostic{"plugin_partial_block_unsupported","ABI v1 requires full-block consumption and production"};
        return AudioBlockResult{b.frame_count};
    }
    void reset() noexcept override { module_->api.reset(instance_); maximum_=0; }
private:
    std::shared_ptr<Module> module_;
    NodeDescriptor desc_;
    aa_instance instance_=nullptr;
    std::vector<aa_input_v1> inputs_;
    std::vector<aa_output_v1> outputs_;
    uint32_t maximum_=0,channels_=0,rate_=0;
};
}

std::vector<std::filesystem::path> configured_plugin_roots() {
    std::vector<std::filesystem::path> result;
    const auto env=juce::SystemStats::getEnvironmentVariable("AUDIOALGO_PLUGIN_PATH",{});
    if(env.isNotEmpty()) {
        for(auto& s:juce::StringArray::fromTokens(env,";","\"")) if(s.trim().isNotEmpty()) result.push_back(std::filesystem::u8path(s.trim().toStdString()));
    } else {
        result.push_back(std::filesystem::u8path(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("plugins").getFullPathName().toStdString()));
        result.push_back(std::filesystem::u8path(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("AudioAlgo/plugins").getFullPathName().toStdString()));
    }
    return result;
}

PluginDiscovery load_native_plugins(NodeRegistry& registry,const std::vector<std::filesystem::path>& roots) {
    PluginDiscovery result;
    std::set<std::filesystem::path> paths;
    for(auto& root:roots) {
        std::error_code ec;
        if(!std::filesystem::is_directory(root,ec)) continue;
        if(std::filesystem::is_regular_file(root/"manifest.json",ec)) paths.insert(std::filesystem::weakly_canonical(root/"manifest.json",ec));
        for(std::filesystem::directory_iterator it(root,ec),end;!ec && it!=end;it.increment(ec)) {
            if(it->is_directory(ec) && std::filesystem::is_regular_file(it->path()/"manifest.json",ec))
                paths.insert(std::filesystem::weakly_canonical(it->path()/"manifest.json",ec));
        }
        if(ec) result.diagnostics.push_back({"plugin_scan_failed","",ec.message(),root});
    }
    std::set<std::string> plugin_ids;
    for(auto& path:paths) {
        std::string id;
        try {
            auto manifest_file=file(path);
            require(manifest_file.getSize()>0 && manifest_file.getSize()<=1024*1024,"Manifest size must be 1..1048576 bytes");
            const auto raw=manifest_file.loadFileAsString();
            auto manifest=juce::JSON::parse(raw);
            require(manifest.isObject(),"Invalid JSON object");
            require(positive(manifest["schema_version"])==1,"Unsupported manifest schema");
            id=string_field(manifest,"plugin_id");
            const auto version=string_field(manifest,"version");
            if(positive(manifest["abi_version"])!=AA_NODE_ABI_V1) throw Rejected("plugin_abi_incompatible","Expected ABI v1");
            if(string_field(manifest,"platform")!="windows" || string_field(manifest,"architecture")!="x64" || sizeof(void*)!=8)
                throw Rejected("plugin_platform_incompatible","Expected Windows x64");
            if(plugin_ids.contains(id)) throw Rejected("plugin_id_duplicate","Plugin ID already loaded");
            auto relative=std::filesystem::u8path(string_field(manifest,"library"));
            require(!relative.is_absolute() && !relative.has_root_path(),"Library must be relative to package");
            auto library=std::filesystem::weakly_canonical(path.parent_path()/relative);
            const auto inside=library.lexically_relative(path.parent_path());
            require(!inside.empty() && *inside.begin()!="..","Library must remain inside package");
            if(!std::filesystem::is_regular_file(library)) throw Rejected("plugin_library_missing","Declared DLL does not exist");
            const auto dll_hash=juce::SHA256(file(library)).toHexString().toStdString();
            const auto manifest_hash=juce::SHA256(manifest_file).toHexString().toStdString();
            std::vector<NodeDescriptor> descriptors;
            for(auto& node:array_field(manifest,"nodes")) {
                auto d=descriptor(node);
                d.plugin_origin=NodeDescriptor::PluginOrigin{id,version,dll_hash,manifest_hash};
                descriptors.push_back(std::move(d));
            }
            require(!descriptors.empty(),"Package must declare at least one node");
            auto module=std::make_shared<Module>();
#ifdef _WIN32
            module->handle=LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if(!module->handle) throw Rejected("plugin_library_load_failed","DLL/dependency or machine format load failed; Win32="+std::to_string(GetLastError()));
            auto entry=reinterpret_cast<aa_get_api_v1>(GetProcAddress(module->handle,AA_NODE_ENTRY));
            if(!entry) throw Rejected("plugin_entry_missing",AA_NODE_ENTRY);
            aa_error e{sizeof(e)};
            if(entry(AA_NODE_ABI_V1,sizeof(module->api),&module->api,&e)!=0)
                throw Rejected("plugin_api_rejected",bounded(e.message));
#else
            throw Rejected("plugin_platform_incompatible","Native plugin loader currently supports Windows x64 only");
#endif
            auto& api=module->api;
            if(api.struct_size!=sizeof(api)||api.abi_version!=AA_NODE_ABI_V1 || !api.create||!api.prepare||!api.process||!api.reset||!api.destroy)
                throw Rejected("plugin_api_incompatible","Invalid ABI table version, size or mandatory function");
            require(api.manifest_json.data && api.manifest_json.size>0 && api.manifest_json.size<=1024*1024,"Invalid DLL descriptor storage");
            const auto exported=juce::JSON::parse(juce::String::fromUTF8(api.manifest_json.data,static_cast<int>(api.manifest_json.size)));
            if(juce::JSON::toString(exported,true)!=juce::JSON::toString(manifest,true))
                throw Rejected("plugin_descriptor_mismatch","DLL metadata differs from deployed manifest");
            // Copy-on-commit keeps even multi-node packages atomic. Factory/instance leases
            // retain module ownership independently of discovery and registry object lifetimes.
            auto candidate=registry;
            for(auto& d:descriptors) {
                auto registration=candidate.register_node(d,[module,d]{return std::make_unique<Processor>(module,d);});
                if(!registration.registered) throw Rejected("plugin_node_registration_failed",registration.diagnostic?registration.diagnostic->message:"Registration failed");
            }
            registry=std::move(candidate);
            plugin_ids.insert(id); result.loaded_plugin_ids.push_back(id);
        } catch(const Rejected& e) { result.diagnostics.push_back({e.code,id,e.what(),path}); }
          catch(const std::exception& e) { result.diagnostics.push_back({"plugin_discovery_failed",id,e.what(),path}); }
    }
    return result;
}

std::string node_catalog_json(const NodeRegistry& registry,const PluginDiscovery& discovery) {
    juce::DynamicObject::Ptr root=new juce::DynamicObject;
    juce::Array<juce::var> nodes,errors;
    for(auto& [id,d]:registry.descriptors()) {
        juce::DynamicObject::Ptr n=new juce::DynamicObject;
        n->setProperty("type_id",js(id)); n->setProperty("version",js(d.version)); n->setProperty("display_name",js(d.display_name));
        juce::Array<juce::var> ports,params;
        for(auto& p:d.ports) {juce::DynamicObject::Ptr v=new juce::DynamicObject; v->setProperty("id",js(p.id));v->setProperty("direction",p.direction==PortDirection::input?"input":"output");v->setProperty("type_id",js(p.type_id));ports.add(juce::var(v.get()));}
        for(auto& p:d.parameters) {juce::DynamicObject::Ptr v=new juce::DynamicObject;v->setProperty("id",js(p.id));params.add(juce::var(v.get()));}
        n->setProperty("ports",ports);n->setProperty("parameters",params);
        if(d.plugin_origin) {auto& o=*d.plugin_origin;n->setProperty("plugin_id",js(o.plugin_id));n->setProperty("library_sha256",js(o.library_sha256));n->setProperty("manifest_sha256",js(o.manifest_sha256));}
        nodes.add(juce::var(n.get()));
    }
    for(auto& e:discovery.diagnostics) {juce::DynamicObject::Ptr v=new juce::DynamicObject;v->setProperty("code",js(e.code));v->setProperty("plugin_id",js(e.plugin_id));v->setProperty("message",js(e.message));v->setProperty("manifest",file(e.manifest).getFullPathName());errors.add(juce::var(v.get()));}
    root->setProperty("nodes",nodes);root->setProperty("diagnostics",errors);
    return juce::JSON::toString(juce::var(root.get()),true).toStdString();
}
}
