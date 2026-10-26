#include "MixedExecution.h"
#include "audio_runtime/PreparedPlan.h"
#include "audio_core/AudioTimeline.h"
#include "audio_io/WavFile.h"
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <set>

namespace audioalgo {
namespace {
using Address=std::pair<std::string,std::string>;
juce::String text(const std::string& value){return juce::String::fromUTF8(value.c_str());}
Address key(const PlanPortAddress& a){return {a.node_id,a.port_id};}
struct Capture {std::vector<AudioBuffer> buffers;std::size_t offset=0;};
class CaptureProcessor final:public AudioNodeProcessor {
public:
    explicit CaptureProcessor(std::shared_ptr<Capture> capture):capture_(std::move(capture)){}
    NodeProcessOutcome process(const NodeProcessRequest&) override{return Diagnostic{"capture_owned_unsupported","Internal capture uses native blocks"};}
    bool supports_block_processing() const noexcept override{return true;}
    bool supports_named_block_processing() const noexcept override{return true;}
    AudioBlockOutcome process_named_block(const NamedAudioBlockView& b) override {
        if(b.inputs.size()!=capture_->buffers.size() || b.outputs.size()!=1)return Diagnostic{"capture_ports_invalid","Invalid materialization projection"};
        for(std::size_t p=0;p<b.inputs.size();++p) {
            const auto cursor=std::min(capture_->offset,capture_->buffers[p].frame_count());
            const auto count=std::min(capture_->buffers[p].frame_count()-cursor,b.frame_count);
            for(std::size_t c=0;c<b.inputs[p].channels.size();++c)
                std::copy_n(b.inputs[p].channels[c],count,capture_->buffers[p].mutable_channel(c).data()+cursor);
        }
        for(std::size_t c=0;c<b.outputs[0].channels.size();++c)std::copy_n(b.inputs[0].channels[c],b.frame_count,b.outputs[0].channels[c]);
        capture_->offset+=b.frame_count;return AudioBlockResult{b.frame_count};
    }
private:std::shared_ptr<Capture> capture_;
};
juce::var port_metadata(const Address& address,const AudioBuffer& audio) {
    juce::DynamicObject::Ptr value=new juce::DynamicObject;value->setProperty("node_id",juce::String::fromUTF8(address.first.c_str()));value->setProperty("port_id",juce::String::fromUTF8(address.second.c_str()));
    value->setProperty("sample_rate_hz",static_cast<int>(audio.sample_rate_hz()));value->setProperty("frames",static_cast<juce::int64>(audio.frame_count()));value->setProperty("dtype","float32");
    value->setProperty("valid_start_frame",static_cast<juce::int64>(audio.valid_interval().first_frame));value->setProperty("valid_frame_count",static_cast<juce::int64>(audio.valid_interval().frame_count));
    juce::Array<juce::var> ids;for(const auto& id:audio.channel_ids())ids.add(juce::String::fromUTF8(id.c_str()));value->setProperty("channel_ids",ids);return juce::var(value.get());
}
}
AudioGraphOutcome render_prepared_audio_graph(const ProjectModel& project,const NodeRegistry& registry,
    std::vector<AudioSourceBinding> sources,int host,NodeExecutionContext context,NativeRenderStatistics* statistics,std::string* manifest_json,std::span<const int> host_partitions) {
    if(auto* messages=juce::MessageManager::getInstanceWithoutCreating();messages && messages->isThisTheMessageThread())
        return Diagnostic{"offline_thread_invalid","Owned offline tasks must run on a background worker"};
    if(sources.empty())return Diagnostic{"source_binding_missing","Mixed plan requires a bound PCM source"};
    const auto& reference=sources.front().audio;
    for(const auto& source:sources) {
        const auto& a=source.audio;
        if(!a.format_error().empty() || a.format()!=reference.format() || a.channel_ids()!=reference.channel_ids() || a.frame_count()!=reference.frame_count() || a.valid_interval()!=reference.valid_interval())
            return Diagnostic{"source_format_mismatch","Mixed sources must have matching format, channel identity, frames and valid interval",source.node_id,"audio"};
    }
    auto compiled=compile_prepared_plan(project,registry,reference.format(),reference.frame_count(),host);
    if(const auto* e=std::get_if<Diagnostic>(&compiled))return *e;
    const auto& plan=std::get<PreparedPlan>(compiled);
    for(const auto partition:host_partitions)if(partition<=0 || partition>host)
        return Diagnostic{"invalid_host_partition","Host partitions must be positive and fit the declared maximum block"};
    // Known latency is removed once per region. A finite retained tail extends
    // the owned PCM interval. Independent output tails need per-port admission.
    for(const auto& task:plan.tasks)if(task.kind==PreparedTaskKind::native_region) {
        std::set<std::string> members;bool tailed=false;
        for(const auto& node:task.nodes){members.insert(node.node_id);tailed=tailed || *node.descriptor.capabilities.tail_frames!=0;}
        if(tailed) {
            std::set<Address> outputs;
            for(const auto& route:plan.routes)if(members.contains(route.from.node_id) && !members.contains(route.to.node_id))outputs.insert(key(route.from));
            if(plan.selected_output && members.contains(plan.selected_output->node_id))outputs.insert(key(*plan.selected_output));
            if(outputs.size()!=1)for(const auto& address:outputs) {
                const auto owner=std::find_if(task.nodes.begin(),task.nodes.end(),[&](const auto& n){return n.node_id==address.first;});
                if(owner->descriptor.capabilities.output_tail_frames.empty())
                    return Diagnostic{"mixed_tail_ports_unsupported","Independent retained output tails require a per-port boundary profile",owner->node_id,address.second};
            }
        }
    }
    std::map<Address,AudioBuffer> values;std::optional<AudioBuffer> terminal;juce::Array<juce::var> records;
    std::size_t total_drain_padding=0;
    const auto cancelled=[&]{return context.is_cancelled && context.is_cancelled();};
    context.frozen_task_packages=std::make_shared<std::map<std::string,std::string>>();
    std::map<std::string,std::unique_ptr<AudioNodeProcessor>,std::less<>> offline_processors;
    // Freeze the whole closure before the first source/native task; instances of
    // one package share the same captured version even if originals later disappear.
    for(const auto& task:plan.tasks)if(task.kind==PreparedTaskKind::offline)for(const auto& n:task.nodes) {
        if(cancelled())return Diagnostic{"run_cancelled","Mixed run cancelled before code freeze"};
        // This profile preserves the PCM shape, so reject a known aggregate
        // offline budget before upstream native materialization or code capture.
        std::size_t port_count=0;
        for(const auto& route:plan.routes)port_count+=route.to.node_id==n.node_id;
        for(const auto& port:n.descriptor.ports)port_count+=port.direction==PortDirection::output;
        if(reference.frame_count()>context.maximum_task_payload_bytes/(4*reference.channel_count()))
            return Diagnostic{"python_payload_limit","Offline input exceeds task byte budget",n.node_id};
        const auto port_bytes=reference.frame_count()*reference.channel_count()*4;
        if(port_bytes && port_count>context.maximum_task_payload_bytes/port_bytes)
            return Diagnostic{"python_payload_limit","Aggregate offline ports exceed task byte budget",n.node_id};
        auto made=registry.create_processor(n.descriptor.type_id,n.node_id);
        if(!made.processor)return Diagnostic{made.diagnostic?made.diagnostic->code:"node_factory_failed",made.diagnostic?made.diagnostic->message:"Offline factory returned no processor",n.node_id};
        auto task_context=context;task_context.node_id=n.node_id;
        if(const auto error=made.processor->freeze_task(task_context))return *error;
        offline_processors.emplace(n.node_id,std::move(made.processor));
    }
    for(std::size_t ti=0;ti<plan.tasks.size();++ti) {
        if(cancelled())return Diagnostic{"run_cancelled","Mixed task cancelled before execution"};
        const auto& task=plan.tasks[ti];std::set<std::string> members;for(const auto& n:task.nodes)members.insert(n.node_id);
        juce::DynamicObject::Ptr record=new juce::DynamicObject;record->setProperty("index",static_cast<int>(ti));
        record->setProperty("kind",task.kind==PreparedTaskKind::source?"source":task.kind==PreparedTaskKind::sink?"sink":task.kind==PreparedTaskKind::offline?"offline":"native_region");
        juce::Array<juce::var> ids,deps,ports;for(const auto& n:task.nodes)ids.add(juce::String::fromUTF8(n.node_id.c_str()));for(const auto dep:task.dependencies)deps.add(static_cast<int>(dep));record->setProperty("nodes",ids);record->setProperty("dependencies",deps);
        if(task.kind==PreparedTaskKind::source) {
            for(const auto& n:task.nodes) {
                const auto source=std::find_if(sources.begin(),sources.end(),[&](const auto& s){return s.node_id==n.node_id;});
                if(source==sources.end())return Diagnostic{"source_binding_missing","No PCM binding for source",n.node_id};
                std::vector<std::string> outputs;for(const auto& p:n.descriptor.ports)if(p.direction==PortDirection::output)outputs.push_back(p.id);
                if(outputs.size()!=1)return Diagnostic{"source_ports_unsupported","This materialized source profile requires one PCM output",n.node_id};
                values.emplace(Address{n.node_id,outputs[0]},source->audio);
            }
        } else if(task.kind==PreparedTaskKind::native_region) {
            juce::Array<juce::var> partitions;for(const auto frames:host_partitions)partitions.add(frames);
            record->setProperty("host_partitions",partitions);
            auto local=registry;ProjectModel projection;projection.project_id=project.project_id;projection.revision=project.revision;
            for(const auto& n:task.nodes)projection.nodes.push_back({n.node_id,n.descriptor.type_id,n.parameters});
            std::set<std::string> reserved;for(const auto& n:project.nodes)reserved.insert(n.id);
            const auto unique=[&](std::string value){while(reserved.contains(value))value+="_";reserved.insert(value);return value;};
            auto source_type=std::string("__audioalgo.materialized_source");while(local.find(source_type))source_type+="_";
            NodeDescriptor source_descriptor{source_type,"1","Materialized source",{{"audio","audio.pcm@1",PortDirection::output}}, {},NodeExecutionKind::source};
            if(!local.register_descriptor(source_descriptor))return Diagnostic{"projection_registration_failed","Could not register materialized source"};
            std::vector<AudioSourceBinding> inputs;std::map<Address,std::string> ingress;std::set<Address> outputs;
            for(const auto& route:plan.routes) {
                const bool from=members.contains(route.from.node_id),to=members.contains(route.to.node_id);
                if(from && to)projection.connections.push_back({route.from.node_id,route.from.port_id,route.to.node_id,route.to.port_id});
                if(to && !from) {
                    const auto address=key(route.from);if(!ingress.contains(address)) {
                        const auto id=unique("__source"+std::to_string(ingress.size()));ingress[address]=id;
                        projection.nodes.push_back({id,source_type,{}});inputs.push_back({id,values.at(address)});
                    }
                    projection.connections.push_back({ingress.at(address),"audio",route.to.node_id,route.to.port_id});
                }
                if(from && !to)outputs.insert(key(route.from));
            }
            if(plan.selected_output && members.contains(plan.selected_output->node_id))outputs.insert(key(*plan.selected_output));
            if(outputs.empty())return Diagnostic{"projection_output_missing","Native region has no materialized output"};
            if(ingress.empty())return Diagnostic{"source_binding_missing","Native region has no materialized input"};
            const auto& region_reference=values.at(ingress.begin()->first);
            for(const auto& input:inputs)if(input.audio.frame_count()!=region_reference.frame_count() || input.audio.valid_interval()!=region_reference.valid_interval())
                return Diagnostic{"mixed_input_timeline_mismatch","Native boundary inputs require matching explicit intervals",input.node_id};
            const auto capture=std::make_shared<Capture>();NodeDescriptor capture_descriptor;capture_descriptor.type_id="__audioalgo.materialized_capture";
            while(local.find(capture_descriptor.type_id))capture_descriptor.type_id+="_";
            capture_descriptor.version="1";capture_descriptor.display_name="Materialized output";capture_descriptor.capabilities.supports_named_audio_ports=true;
            const auto capture_id=unique("__capture");std::size_t ordinal=0;
            for(const auto& address:outputs) {
                const auto port="input"+std::to_string(ordinal++);capture_descriptor.ports.push_back({port,"audio.pcm@1",PortDirection::input,true});
                projection.connections.push_back({address.first,address.second,capture_id,port});
                capture->buffers.emplace_back(reference.sample_rate_hz(),reference.channel_ids(),0);
            }
            capture_descriptor.ports.push_back({"audio","audio.pcm@1",PortDirection::output});
            const auto registered=local.register_node(capture_descriptor,[capture]{return std::make_unique<CaptureProcessor>(capture);});
            if(!registered.registered)return Diagnostic{"projection_registration_failed","Could not register materialized capture"};
            projection.nodes.push_back({capture_id,capture_descriptor.type_id,{}});projection.selected_output_node_id=capture_id;projection.selected_output_port_id="audio";
            const auto native=compile_native_plan(projection,local,region_reference.format(),region_reference.frame_count(),host);
            if(const auto* e=std::get_if<Diagnostic>(&native))return *e;
            const auto& timeline=std::get<NativePlan>(native);const auto latency=timeline.latency_frames;
            const auto aligned_frames=timeline.source_frames+timeline.retained_tail_frames;
            const auto raw_frames=aligned_frames+latency;
            const auto frame_budget=context.maximum_task_payload_bytes/(4*reference.channel_count())/outputs.size();
            if(aligned_frames>frame_budget || raw_frames>frame_budget-aligned_frames)
                return Diagnostic{"python_payload_limit","Native boundary including padding exceeds task byte budget",task.nodes.front().node_id};
            ordinal=0;for(const auto& address:outputs) {
                const auto owner=std::find_if(timeline.nodes.begin(),timeline.nodes.end(),[&](const auto& n){return n.node_id==address.first;});
                const auto tail=owner->output_tails.at(address.second);auto port_valid=region_reference.valid_interval();
                if(port_valid.frame_count && port_valid.first_frame+port_valid.frame_count==timeline.source_frames)port_valid.frame_count+=tail;
                capture->buffers[ordinal++]=AudioBuffer(reference.sample_rate_hz(),reference.channel_ids(),timeline.source_frames+tail+latency,
                    {latency+port_valid.first_frame,port_valid.frame_count});
            }
            NativeRenderStatistics region_statistics;
            const auto rendered=render_audio_graph(projection,local,std::move(inputs),host,{host_partitions,&region_statistics,false,context.is_cancelled,context});
            if(const auto* e=std::get_if<Diagnostic>(&rendered))return *e;
            if(region_statistics.drain_padding_frames>std::numeric_limits<std::size_t>::max()-total_drain_padding)
                return Diagnostic{"render_length_overflow","Aggregate native drain count exceeds the sample range"};
            total_drain_padding+=region_statistics.drain_padding_frames;
            if(capture->offset!=raw_frames || region_statistics.alignment_offset_frames!=latency)
                return Diagnostic{"capture_frame_mismatch","Native materialization did not cover the declared raw timeline"};
            juce::DynamicObject::Ptr alignment=new juce::DynamicObject;
            alignment->setProperty("offset_frames",static_cast<juce::int64>(latency));alignment->setProperty("raw_frames",static_cast<juce::int64>(raw_frames));
            alignment->setProperty("aligned_frames",static_cast<juce::int64>(aligned_frames));
            alignment->setProperty("source_frames",static_cast<juce::int64>(timeline.source_frames));
            alignment->setProperty("retained_tail_frames",static_cast<juce::int64>(timeline.retained_tail_frames));
            alignment->setProperty("drain_padding_frames",static_cast<juce::int64>(region_statistics.drain_padding_frames));
            juce::Array<juce::var> artifacts;
            ordinal=0;for(const auto& address:outputs) {
                const auto owner=std::find_if(timeline.nodes.begin(),timeline.nodes.end(),[&](const auto& n){return n.node_id==address.first;});
                const auto port_tail=owner->output_tails.at(address.second);
                const auto port_frames=timeline.source_frames+port_tail;
                auto port_valid=region_reference.valid_interval();
                if(port_valid.frame_count && port_valid.first_frame+port_valid.frame_count==timeline.source_frames)port_valid.frame_count+=port_tail;
                const auto& raw=capture->buffers[ordinal];
                AudioBuffer aligned(reference.sample_rate_hz(),reference.channel_ids(),port_frames,port_valid);
                for(std::size_t c=0;c<reference.channel_count();++c) {
                    std::copy_n(raw.channel(c).data()+latency,port_frames,aligned.mutable_channel(c).data());
                }
                if(latency && !context.output_directory.empty()) {
                    const auto folder=std::filesystem::path("native-regions")/std::to_string(ti);std::error_code error;
                    std::filesystem::create_directories(std::filesystem::u8path(context.output_directory)/folder,error);
                    if(error)return Diagnostic{"boundary_output_create_failed",error.message(),address.first,address.second};
                    juce::DynamicObject::Ptr artifact=new juce::DynamicObject;artifact->setProperty("node_id",text(address.first));artifact->setProperty("port_id",text(address.second));
                    artifact->setProperty("raw_valid_start_frame",static_cast<juce::int64>(latency+port_valid.first_frame));artifact->setProperty("valid_start_frame",static_cast<juce::int64>(port_valid.first_frame));
                    artifact->setProperty("valid_frame_count",static_cast<juce::int64>(port_valid.frame_count));
                    artifact->setProperty("raw_frames",static_cast<juce::int64>(port_frames+latency));artifact->setProperty("aligned_frames",static_cast<juce::int64>(port_frames));
                    for(const auto& [kind,audio]:std::vector<std::pair<std::string,const AudioBuffer*>>{{"raw",&raw},{"aligned",&aligned}}) {
                        if(cancelled())return Diagnostic{"run_cancelled","Cancelled before native boundary publication"};
                        const auto relative=folder/("port-"+std::to_string(ordinal)+"."+kind+".wav"),path=std::filesystem::u8path(context.output_directory)/relative;
                        const auto written=write_wav_file(path,*audio,WavEncoding::float32);if(const auto* e=std::get_if<Diagnostic>(&written))return *e;
                        const auto utf8=path.u8string();const auto file=juce::File(juce::String::fromUTF8(reinterpret_cast<const char*>(utf8.data()),static_cast<int>(utf8.size())));
                        artifact->setProperty(juce::Identifier(text(kind+"_path")),text(relative.generic_string()));artifact->setProperty(juce::Identifier(text(kind+"_sha256")),juce::SHA256(file).toHexString());
                    }
                    artifacts.add(juce::var(artifact.get()));
                }
                values.emplace(address,std::move(aligned));++ordinal;
            }
            alignment->setProperty("artifacts",artifacts);record->setProperty("alignment",juce::var(alignment.get()));
        } else {
            const auto& n=task.nodes.front();AudioPorts inputs;
            const AudioBuffer* boundary_reference=nullptr;
            for(const auto& route:plan.routes)if(route.to.node_id==n.node_id) {
                const auto& audio=values.at(key(route.from));
                if(!boundary_reference)boundary_reference=&audio;
                else if(audio.frame_count()!=boundary_reference->frame_count() || audio.valid_interval()!=boundary_reference->valid_interval())
                    return Diagnostic{"mixed_input_timeline_mismatch","Offline boundary inputs require matching explicit intervals",n.node_id,route.to.port_id};
            }
            if(!boundary_reference)return Diagnostic{"source_binding_missing","Offline boundary has no owned PCM input",n.node_id};
            // Validate the aggregate task budget before copying any owned port.
            std::size_t bytes=0,outputs=0;for(const auto& p:n.descriptor.ports)outputs+=p.direction==PortDirection::output;
            for(const auto& route:plan.routes)if(route.to.node_id==n.node_id) {
                const auto& a=values.at(key(route.from));
                if(a.channel_count()==0 || a.frame_count()>context.maximum_task_payload_bytes/(4*a.channel_count()))return Diagnostic{"python_payload_limit","Input exceeds task byte budget",n.node_id,route.to.port_id};
                const auto count=a.frame_count()*a.channel_count()*4;
                if(count>context.maximum_task_payload_bytes-bytes)return Diagnostic{"python_payload_limit","Aggregate inputs exceed task byte budget",n.node_id,route.to.port_id};bytes+=count;
            }
            const auto output_bytes=boundary_reference->frame_count()*boundary_reference->channel_count()*4;
            if(task.kind==PreparedTaskKind::offline && outputs && output_bytes>(context.maximum_task_payload_bytes-bytes)/outputs)
                return Diagnostic{"python_payload_limit","Aggregate task outputs exceed byte budget",n.node_id};
            for(const auto& route:plan.routes)if(route.to.node_id==n.node_id)inputs.emplace(route.to.port_id,values.at(key(route.from)));
            if(task.kind==PreparedTaskKind::sink) {
                if(inputs.size()!=1)return Diagnostic{"sink_ports_unsupported","Selected PCM sink requires one input",n.node_id};
                terminal=inputs.begin()->second;
            } else {
                auto task_context=context;task_context.node_id=n.node_id;task_context.maximum_block_frames=host;
                auto result=offline_processors.at(n.node_id)->process({n.parameters,inputs,task_context});if(const auto* e=std::get_if<Diagnostic>(&result))return *e;
                auto audio=std::move(std::get<AudioPorts>(result));
                if(audio.size()!=outputs)return Diagnostic{"offline_output_ports_invalid","Offline output map differs from descriptor",n.node_id};
                for(const auto& p:n.descriptor.ports)if(p.direction==PortDirection::output) {
                    auto found=audio.find(p.id);if(found==audio.end())return Diagnostic{"offline_output_ports_invalid","Missing declared output",n.node_id,p.id};
                    const auto& a=found->second;
                    if(!a.format_error().empty() || a.format()!=boundary_reference->format() || a.frame_count()!=boundary_reference->frame_count() || a.channel_ids()!=boundary_reference->channel_ids() || a.valid_interval()!=boundary_reference->valid_interval())
                        return Diagnostic{"offline_output_format_invalid","Output changed the declared PCM boundary",n.node_id,p.id};
                    for(std::size_t c=0;c<a.channel_count();++c)for(const auto sample:a.channel(c))if(!std::isfinite(sample))return Diagnostic{"output_non_finite","Offline output contains nonfinite PCM",n.node_id,p.id};
                    values.emplace(Address{n.node_id,p.id},std::move(found->second));
                }
            }
        }
        if(cancelled())return Diagnostic{"run_cancelled","Mixed task cancelled before materialization acceptance"};
        for(const auto& [address,audio]:values)if(members.contains(address.first))ports.add(port_metadata(address,audio));
        record->setProperty("outputs",ports);records.add(juce::var(record.get()));
    }
    if(plan.selected_output)terminal=values.at(key(*plan.selected_output));
    if(!terminal)return Diagnostic{"selected_output_missing","Mixed plan did not produce the selected terminal"};
    if(terminal->frame_count()<reference.frame_count())return Diagnostic{"mixed_output_timeline_invalid","Owned output unexpectedly shortened the logical stream"};
    if(statistics){*statistics={};statistics->source_frames=reference.frame_count();statistics->raw_frames=statistics->aligned_frames=terminal->frame_count();
        statistics->retained_tail_frames=terminal->frame_count()-reference.frame_count();
        statistics->drain_padding_frames=total_drain_padding;
        statistics->raw_duration_ns=statistics->aligned_duration_ns=map_sample_frame(terminal->frame_count(),terminal->sample_rate_hz(),1000000000).value_or(0);}
    if(manifest_json)*manifest_json=juce::JSON::toString(records,true).toStdString();
    return std::move(*terminal);
}
}
