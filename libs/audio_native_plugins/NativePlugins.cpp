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
#include <fstream>
#include <regex>
#ifdef AUDIOALGO_HAS_PYTHON
#include "audio_python_backend/PythonTask.h"
#endif
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
std::string manifest_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);require(stream.is_open(),"Could not read manifest");
    std::string data(1024*1024+1,'\0');stream.read(data.data(),static_cast<std::streamsize>(data.size()));
    const auto count=static_cast<std::size_t>(stream.gcount());require(!stream.bad() && count>0 && count<=1024*1024,"Manifest size must be1..1048576 bytes");data.resize(count);return data;
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
    if(boolean_field(v,"requires_offline_task",false))
        throw Rejected("plugin_capability_unsupported","Published C ABI v1 does not expose owned offline tasks");
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
NodeDescriptor python_descriptor(const juce::var& node) {
    const auto bounded_text=[](const juce::var& object,const char* key) {
        const auto value=string_field(object,key);require(value.size()<=1024,key);return value;
    };
    const auto identifier=[&](const juce::var& object,const char* key) {
        const auto value=bounded_text(object,key);
        static const std::regex id(R"([A-Za-z0-9_][A-Za-z0-9_.-]{0,127})");
        require(std::regex_match(value,id),key);return value;
    };
    identifier(node,"type_id");bounded_text(node,"version");bounded_text(node,"display_name");
    const auto execution=node["execution"];require(execution.isObject(),"Python execution must be an object");
    const auto kind=string_field(execution,"kind");require(kind=="block" || kind=="clip","Python kind must be block or clip");
    require(!boolean_field(execution,"realtime_verified",false),"Python cannot execute in the device callback");
    (void)boolean_field(execution,"stateful",false);
    const auto entry=string_field(node,"entrypoint");
    static const std::regex pattern(R"([A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)*:[A-Za-z_][A-Za-z0-9_]*)");
    require(entry.size()<=1024 && std::regex_match(entry,pattern),"Invalid package-relative Python entrypoint");
    juce::DynamicObject::Ptr normalized=new juce::DynamicObject;
    for(const auto* key:{"type_id","version","display_name"})normalized->setProperty(key,node[key]);
    juce::Array<juce::var> ports,parameters;
    for(const auto* direction:{"inputs","outputs"}) {
        const auto& declared=array_field(node,direction);require(!declared.isEmpty() && declared.size()<=64,"Python needs1..64 ports per direction");
        for(const auto& p:declared){juce::DynamicObject::Ptr value=new juce::DynamicObject;
            identifier(p,"id");
            value->setProperty("id",p["id"]);value->setProperty("type_id",p["type"]);value->setProperty("direction",std::string_view(direction)=="inputs"?"input":"output");
            value->setProperty("required",boolean_field(p,"required",false));ports.add(juce::var(value.get()));}
    }
    const auto& declared=array_field(node,"parameters");require(declared.size()<=128,"Python parameter count exceeds128");
    for(const auto& p:declared){require(p.isObject(),"Parameter must be an object");
        identifier(p,"id");require(p.hasProperty("default"),"Parameter default is required");
        if(p.hasProperty("update_policy"))require(string_field(p,"update_policy")=="OfflineOnly","Python dynamic parameter updates are unsupported");
        juce::DynamicObject::Ptr value=new juce::DynamicObject;const auto& properties=p.getDynamicObject()->getProperties();
        for(int i=0;i<properties.size();++i)value->setProperty(properties.getName(i),properties.getValueAt(i));
        value->setProperty("kind",p["type"]);parameters.add(juce::var(value.get()));}
    normalized->setProperty("ports",ports);normalized->setProperty("parameters",parameters);
    for(const auto* name:{"sample_rates_hz","channels"}) {
        const auto& values=array_field(node["format"],name);
        require(!values.isEmpty() && values.size()<=32,"Python format needs1..32 values");
    }
    normalized->setProperty("sample_rates_hz",node["format"]["sample_rates_hz"]);normalized->setProperty("channel_counts",node["format"]["channels"]);
    auto result=descriptor(juce::var(normalized.get()));result.capabilities.requires_offline_task=true;
    result.backend_metadata_json=juce::JSON::toString(node,true).toStdString();return result;
}
class PythonProcessor final:public AudioNodeProcessor {
public:
    PythonProcessor(std::filesystem::path root,NodeDescriptor desc,std::string hash):root_(std::move(root)),desc_(std::move(desc)),manifest_hash_(std::move(hash)){}
    std::optional<Diagnostic> freeze_task(const NodeExecutionContext& context) override {
#ifdef AUDIOALGO_HAS_PYTHON
        configure_runtime();
        interpreter_hash_=juce::SHA256(file(python_executable_)).toHexString().toStdString();
        const auto runner_key="python-runner:"+file(runner_root_).getFullPathName().toStdString();
        const auto existing=context.frozen_task_packages?context.frozen_task_packages->find(runner_key):std::map<std::string,std::string>::iterator{};
        if(context.frozen_task_packages && existing!=context.frozen_task_packages->end())runner_root_=std::filesystem::u8path(existing->second);
        else {
            const auto runner=freeze_python_package(runner_root_,std::filesystem::u8path(context.output_directory)/"runner-packages",{},context.is_cancelled);
            if(const auto* error=std::get_if<Diagnostic>(&runner)){auto e=*error;e.node_id=context.node_id;return e;}
            runner_root_=std::get<std::filesystem::path>(runner);
            if(context.frozen_task_packages)(*context.frozen_task_packages)[runner_key]=file(runner_root_).getFullPathName().toStdString();
        }
        const auto cache_key="python-plugin:"+file(root_).getFullPathName().toStdString()+":"+manifest_hash_;
        if(context.frozen_task_packages)if(const auto found=context.frozen_task_packages->find(cache_key);found!=context.frozen_task_packages->end()){frozen_root_=std::filesystem::u8path(found->second);return {};}
        const auto frozen=freeze_python_package(root_,std::filesystem::u8path(context.output_directory)/"python-packages",manifest_hash_,context.is_cancelled);
        if(const auto* error=std::get_if<Diagnostic>(&frozen)){auto e=*error;e.node_id=context.node_id;return e;}
        frozen_root_=std::get<std::filesystem::path>(frozen);
        if(context.frozen_task_packages)(*context.frozen_task_packages)[cache_key]=file(frozen_root_).getFullPathName().toStdString();
        return {};
#else
        return Diagnostic{"python_runtime_disabled","Python backend disabled",context.node_id};
#endif
    }
    NodeProcessOutcome process(const NodeProcessRequest& request) override {
#ifdef AUDIOALGO_HAS_PYTHON
        const auto execution_root=frozen_root_.empty()?root_:frozen_root_;
        const auto bytes=manifest_bytes(execution_root/"manifest.json");
        if(juce::SHA256(bytes.data(),bytes.size()).toHexString().toStdString()!=manifest_hash_)
            return Diagnostic{"python_manifest_changed","Python metadata changed; refresh the host catalog before running",request.context.node_id,{}};
        if(!frozen_root_.empty())for(const auto& root:{frozen_root_,runner_root_})if(auto error=validate_python_snapshot(root,request.context.is_cancelled)){error->node_id=request.context.node_id;return *error;}
        PythonTaskRequest task;
        if(python_executable_.empty())configure_runtime();
        if(!interpreter_hash_.empty() && juce::SHA256(file(python_executable_)).toHexString().toStdString()!=interpreter_hash_)
            return Diagnostic{"python_environment_changed","Frozen interpreter contents changed before execution",request.context.node_id};
        task.python_executable=python_executable_;task.runner_module_root=runner_root_;
        task.plugin_root=execution_root;task.output_parent=std::filesystem::u8path(request.context.output_directory);task.type_id=desc_.type_id;task.node_id=request.context.node_id;
        task.parameters=request.parameters;task.inputs=request.inputs;task.identity=request.context.identity;task.is_cancelled=request.context.is_cancelled;
        task.maximum_block_frames=request.context.maximum_block_frames;task.maximum_payload_bytes=request.context.maximum_task_payload_bytes;
        for(const auto& port:desc_.ports)if(port.direction==PortDirection::output)task.output_ports.push_back(port.id);
        PythonTaskStatistics statistics;auto outcome=run_python_task(task,&statistics);
        if(const auto* error=std::get_if<Diagnostic>(&outcome))return *error;
        auto success=std::move(std::get<PythonTaskSuccess>(outcome));
        if(!frozen_root_.empty())for(const auto& root:{frozen_root_,runner_root_})if(auto error=validate_python_snapshot(root,request.context.is_cancelled)){error->node_id=request.context.node_id;return *error;}
        if(request.context.record_task_artifact){auto provenance=juce::JSON::parse(js(success.provenance_json));auto* object=provenance.getDynamicObject();
            object->setProperty("process_id",static_cast<juce::int64>(statistics.process_id));object->setProperty("node_id",js(request.context.node_id));object->setProperty("request_id",js(task.identity.request_id));
            object->setProperty("interpreter_sha256",js(interpreter_hash_));
            juce::DynamicObject::Ptr logs=new juce::DynamicObject;
            logs->setProperty("stdout_bytes",static_cast<juce::int64>(statistics.stdout_bytes));logs->setProperty("stderr_bytes",static_cast<juce::int64>(statistics.stderr_bytes));
            logs->setProperty("stdout_tail",js(statistics.stdout_tail));logs->setProperty("stderr_tail",js(statistics.stderr_tail));
            object->setProperty("logs",juce::var(logs.get()));
            object->setProperty("frozen_package_root",file(execution_root).getRelativePathFrom(file(std::filesystem::u8path(request.context.output_directory))).replaceCharacter('\\','/'));
            object->setProperty("runner_snapshot",file(runner_root_).getRelativePathFrom(file(std::filesystem::u8path(request.context.output_directory))).replaceCharacter('\\','/'));
            request.context.record_task_artifact(file(success.artifact_directory).getFullPathName().toStdString(),juce::JSON::toString(provenance,true).toStdString());}
        return std::move(success.outputs);
#else
        return Diagnostic{"python_runtime_disabled","This host configuration disables the Python process backend",request.context.node_id,{}};
#endif
    }
private:
    void configure_runtime() {
#ifdef AUDIOALGO_HAS_PYTHON
        const auto configured=juce::SystemStats::getEnvironmentVariable("AUDIOALGO_PYTHON_EXECUTABLE",{});
        const auto host=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory(),bundled=host.getChildFile("python/python.exe"),modules=host.getChildFile("python");
        python_executable_=std::filesystem::u8path(configured.isNotEmpty()?configured.toStdString():bundled.existsAsFile()?bundled.getFullPathName().toStdString():std::string(AUDIOALGO_PYTHON_EXE_SOURCE));
        runner_root_=std::filesystem::u8path(modules.getChildFile("audioalgo_runner/__main__.py").existsAsFile()?modules.getFullPathName().toStdString():std::string(AUDIOALGO_PYTHON_MODULE_SOURCE));
#endif
    }
    std::filesystem::path root_,frozen_root_,python_executable_,runner_root_;NodeDescriptor desc_;std::string manifest_hash_,interpreter_hash_;
};

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
            const auto captured=manifest_bytes(path);
            const auto raw=juce::String::createStringFromData(captured.data(),static_cast<int>(captured.size()));
            auto manifest=juce::JSON::parse(raw);
            require(manifest.isObject(),"Invalid JSON object");
            require(positive(manifest["schema_version"])==1,"Unsupported manifest schema");
            id=string_field(manifest,"plugin_id");
            const auto manifest_hash=juce::SHA256(captured.data(),captured.size()).toHexString().toStdString();
            if(text(manifest["runtime"])=="python") {
#ifndef AUDIOALGO_HAS_PYTHON
                throw Rejected("python_runtime_disabled","This host configuration disables the Python process backend");
#endif
                const auto version=string_field(manifest,"plugin_version");
                static const std::regex identifier(R"([A-Za-z0-9_][A-Za-z0-9_.-]{0,127})");
                require(std::regex_match(id,identifier) && version.size()<=1024,"Invalid Python plugin ID/version");
                if(plugin_ids.contains(id))throw Rejected("plugin_id_duplicate","Plugin ID already loaded");
                const auto& nodes=array_field(manifest,"nodes");require(!nodes.isEmpty() && nodes.size()<=64,"Python package needs1..64 nodes");
                auto candidate=registry;
                for(const auto& node:nodes){auto d=python_descriptor(node);d.plugin_origin=NodeDescriptor::PluginOrigin{id,version,"",manifest_hash,"python"};
                    if(candidate.find(d.type_id))throw Rejected("python_node_duplicate","Python node type ID already registered");
                    const auto root=path.parent_path();const auto registration=candidate.register_node(d,[root,d,manifest_hash]{return std::make_unique<PythonProcessor>(root,d,manifest_hash);});
                    if(!registration.registered)throw Rejected("python_node_registration_failed",registration.diagnostic?registration.diagnostic->message:"Python registration failed");}
                registry=std::move(candidate);plugin_ids.insert(id);result.loaded_plugin_ids.push_back(id);continue;
            }
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
        n->setProperty("execution_domain",d.capabilities.requires_offline_task?"offline_task":"native_region");
        if(!d.backend_metadata_json.empty())n->setProperty("backend_descriptor",juce::JSON::parse(js(d.backend_metadata_json)));
        if(d.plugin_origin) {auto& o=*d.plugin_origin;n->setProperty("plugin_id",js(o.plugin_id));n->setProperty("runtime",js(o.runtime));n->setProperty("library_sha256",js(o.library_sha256));n->setProperty("manifest_sha256",js(o.manifest_sha256));}
        nodes.add(juce::var(n.get()));
    }
    for(auto& e:discovery.diagnostics) {juce::DynamicObject::Ptr v=new juce::DynamicObject;v->setProperty("code",js(e.code));v->setProperty("plugin_id",js(e.plugin_id));v->setProperty("message",js(e.message));v->setProperty("manifest",file(e.manifest).getFullPathName());errors.add(juce::var(v.get()));}
    root->setProperty("nodes",nodes);root->setProperty("diagnostics",errors);
    return juce::JSON::toString(juce::var(root.get()),true).toStdString();
}
}
