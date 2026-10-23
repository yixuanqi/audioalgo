#include "PreparedPlan.h"
#include <algorithm>
#include <map>
#include <numeric>
#include <set>

namespace audioalgo {
PreparedPlanOutcome compile_prepared_plan(const ProjectModel& project,const NodeCatalog& catalog,
    AudioFormat format,std::size_t frames,int host) {
    if((format.sample_rate_hz!=16000 && format.sample_rate_hz!=48000) ||
       (format.channel_count!=1 && format.channel_count!=2 && format.channel_count!=4))
        return Diagnostic{"invalid_audio_format","Mixed PCM tasks require 16/48 kHz and 1/2/4 channels"};
    if(host<=0)return Diagnostic{"invalid_block_size","Host block size must be positive"};
    std::vector<GraphDiagnostic> diagnostics;const auto order=topological_order(project,catalog,diagnostics);
    if(!diagnostics.empty()){const auto& e=diagnostics.front();return Diagnostic{e.code,e.message,e.node_id,e.port_id};}
    const auto selection=evaluate_plan_selection(project,catalog);
    if(const auto* e=std::get_if<Diagnostic>(&selection))return *e;
    const auto& selected=std::get<PlanSelection>(selection);
    PreparedPlan plan;plan.format=format;plan.source_frames=frames;plan.maximum_block_frames=host;
    plan.terminal_node=selected.terminal_node;plan.selected_output=selected.selected_output;
    std::vector<NativePlanNode> nodes;std::vector<PreparedTaskKind> kinds;
    std::map<std::string,std::size_t,std::less<>> index;
    for(const auto& id:order) {
        if(!selected.closure.contains(id))continue;
        const auto& instance=*std::find_if(project.nodes.begin(),project.nodes.end(),[&](const auto& n){return n.id==id;});
        const auto& d=*catalog.find(instance.type_id);const auto& constraints=d.audio_format;
        if((!constraints.sample_rates_hz.empty() && std::find(constraints.sample_rates_hz.begin(),constraints.sample_rates_hz.end(),format.sample_rate_hz)==constraints.sample_rates_hz.end()) ||
           (!constraints.channel_counts.empty() && std::find(constraints.channel_counts.begin(),constraints.channel_counts.end(),format.channel_count)==constraints.channel_counts.end()))
            return Diagnostic{"capability_unsupported","Task format is not supported by the node",id};
        for(const auto& p:d.ports)if(p.type_id!="audio.pcm@1")
            return Diagnostic{"port_type_unsupported","This task profile requires owned PCM boundaries",id,p.id};
        if(!d.capabilities.requires_offline_task) {
            if(const auto e=validate_native_plan_node(d,id,format,frames,host))return *e;
        } else if(d.execution_kind!=NodeExecutionKind::block || !d.capabilities.latency_frames ||
                  *d.capabilities.latency_frames!=0 || !d.capabilities.tail_frames || *d.capabilities.tail_frames!=0) {
            return Diagnostic{"offline_timeline_unsupported","Offline PCM transforms must preserve the declared timeline in this profile",id};
        }
        index[id]=nodes.size();auto parameters=instance.parameters;
        for(const auto& p:d.parameters)parameters.try_emplace(p.id,p.default_value);
        nodes.push_back({id,d,std::move(parameters)});
        kinds.push_back(d.capabilities.requires_offline_task?PreparedTaskKind::offline:d.execution_kind==NodeExecutionKind::source?PreparedTaskKind::source:d.execution_kind==NodeExecutionKind::sink?PreparedTaskKind::sink:PreparedTaskKind::native_region);
    }
    const auto size=nodes.size();std::vector<std::size_t> group(size);std::iota(group.begin(),group.end(),0);
    const auto root=[&](std::size_t i){while(group[i]!=i)i=group[i];return i;};
    std::vector<std::pair<std::size_t,std::size_t>> edges;
    for(const auto& c:project.connections) {
        if(!index.contains(c.from_node) || !index.contains(c.to_node))continue;
        const auto from=index.at(c.from_node),to=index.at(c.to_node);edges.emplace_back(from,to);
        const auto base=[&](const NodeDescriptor& d,PortDirection direction,const std::string& id)->std::optional<std::size_t> {
            std::size_t ordinal=0;for(const auto& p:d.ports)if(p.direction==direction){if(p.id==id){
                if(ordinal && !d.capabilities.requires_offline_task && d.execution_kind==NodeExecutionKind::block && !d.capabilities.supports_named_audio_ports)return {};
                return ordinal*format.channel_count;}++ordinal;}return {};
        };
        const auto a=base(nodes[from].descriptor,PortDirection::output,c.from_port),b=base(nodes[to].descriptor,PortDirection::input,c.to_port);
        if(!a || !b)return Diagnostic{"port_mapping_unsupported","Additional native PCM groups require named port capability",c.to_node,c.to_port};
        plan.routes.push_back({{c.from_node,c.from_port},{c.to_node,c.to_port},*a,*b,format.channel_count});
    }
    // Only contract native edges whose quotient graph remains a DAG. A bypass
    // A->B alongside A->offline->B must retain two native regions.
    const auto sorted_groups=[&] {
        std::vector<std::set<std::size_t>> outgoing(size);std::vector<std::size_t> incoming(size);
        for(const auto& [a,b]:edges){const auto x=root(a),y=root(b);if(x!=y && outgoing[x].insert(y).second)++incoming[y];}
        std::set<std::size_t> ready;for(std::size_t i=0;i<size;++i)if(root(i)==i && !incoming[i])ready.insert(i);
        std::vector<std::size_t> sorted;
        while(!ready.empty()){const auto i=*ready.begin();ready.erase(ready.begin());sorted.push_back(i);for(const auto j:outgoing[i])if(--incoming[j]==0)ready.insert(j);}
        return sorted;
    };
    std::size_t groups=size;
    for(const auto& [a,b]:edges)if(kinds[a]==PreparedTaskKind::native_region && kinds[b]==PreparedTaskKind::native_region) {
        const auto x=root(a),y=root(b);if(x==y)continue;group[y]=x;
        if(sorted_groups().size()!=groups-1)group[y]=y;else --groups;
    }
    const auto sorted=sorted_groups();if(sorted.size()!=groups)return Diagnostic{"task_cycle","Task partition produced a dependency cycle"};
    std::map<std::size_t,std::size_t> task_index;
    for(const auto g:sorted) {
        task_index[g]=plan.tasks.size();PreparedTask task{kinds[g]};
        for(std::size_t i=0;i<size;++i)if(root(i)==g)task.nodes.push_back(nodes[i]);
        std::set<std::size_t> dependencies;for(const auto& [a,b]:edges)if(root(b)==g && root(a)!=g)dependencies.insert(task_index.at(root(a)));
        task.dependencies.assign(dependencies.begin(),dependencies.end());plan.tasks.push_back(std::move(task));
    }
    return plan;
}
}
