#include "audio_runtime/PreparedPlan.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>

namespace {
struct MixedFixture {
    audioalgo::NodeRegistry catalog;
    audioalgo::ProjectModel project;
    MixedFixture() {
        using namespace audioalgo;
        REQUIRE(catalog.register_descriptor({"fixture.source","1","Source",{{"audio","audio.pcm@1",PortDirection::output}}, {},NodeExecutionKind::source}));
        NodeDescriptor native{"fixture.native","1","Native",{{"in","audio.pcm@1",PortDirection::input,true},{"side","audio.pcm@1",PortDirection::input,false},{"out","audio.pcm@1",PortDirection::output},{"other","audio.pcm@1",PortDirection::output}},{{"level",ParameterKind::floating,0.5}}};
        native.capabilities.supports_named_audio_ports=true;REQUIRE(catalog.register_descriptor(native));
        auto offline=native;offline.type_id="fixture.offline";offline.capabilities.requires_offline_task=true;REQUIRE(catalog.register_descriptor(offline));
        REQUIRE(catalog.register_descriptor({"fixture.sink","1","Sink",{{"in","audio.pcm@1",PortDirection::input,true}}, {},NodeExecutionKind::sink}));
        project.project_id="mixed-plan";project.selected_output_node_id="sink";
        project.nodes={{"sink","fixture.sink",{}},{"b","fixture.native",{}},{"python","fixture.offline",{}},{"a","fixture.native",{}},{"source","fixture.source",{}}};
        project.connections={{"source","audio","a","in"},{"a","other","python","in"},{"python","other","b","in"},{"b","out","sink","in"}};
    }
};
std::size_t task_for(const audioalgo::PreparedPlan& plan,const std::string& id) {
    for(std::size_t i=0;i<plan.tasks.size();++i)for(const auto& n:plan.tasks[i].nodes)if(n.node_id==id)return i;
    FAIL("Missing task for "<<id);return 0;
}
}
TEST_CASE("P3-02 mixed plan partitions capabilities and freezes named routes without factories","[P3-02][pure]") {
    using namespace audioalgo;MixedFixture f;
    const auto old=compile_native_plan(f.project,f.catalog,{48000,2},513,127);
    REQUIRE(std::holds_alternative<Diagnostic>(old));CHECK(std::get<Diagnostic>(old).code=="task_boundary_required");
    const auto outcome=compile_prepared_plan(f.project,f.catalog,{48000,2},513,127);
    if(const auto* e=std::get_if<Diagnostic>(&outcome)){INFO(e->code);INFO(e->message);}
    REQUIRE(std::holds_alternative<PreparedPlan>(outcome));const auto& plan=std::get<PreparedPlan>(outcome);
    REQUIRE(plan.tasks.size()==5);CHECK(plan.tasks[0].kind==PreparedTaskKind::source);
    CHECK(plan.tasks[1].kind==PreparedTaskKind::native_region);CHECK(plan.tasks[2].kind==PreparedTaskKind::offline);
    CHECK(plan.tasks[3].kind==PreparedTaskKind::native_region);CHECK(plan.tasks[4].kind==PreparedTaskKind::sink);
    for(std::size_t i=1;i<plan.tasks.size();++i)CHECK(plan.tasks[i].dependencies==std::vector<std::size_t>{i-1});
    REQUIRE(plan.routes.size()==4);CHECK(plan.routes[1].from.port_id=="other");CHECK(plan.routes[2].to.port_id=="in");
    CHECK(std::get<double>(plan.tasks[2].nodes[0].parameters.at("level"))==0.5);CHECK(f.project.nodes[2].parameters.empty());
    f.project.nodes[2].parameters["level"]=3.0;CHECK(std::get<double>(plan.tasks[2].nodes[0].parameters.at("level"))==0.5);
}
TEST_CASE("P3-02 cross boundary bypass preserves DAG and shared native execution","[P3-02][pure]") {
    using namespace audioalgo;MixedFixture f;f.project.connections.push_back({"a","out","b","side"});
    auto outcome=compile_prepared_plan(f.project,f.catalog,{48000,2},513,1);REQUIRE(std::holds_alternative<PreparedPlan>(outcome));
    const auto& p=std::get<PreparedPlan>(outcome);CHECK(task_for(p,"a")!=task_for(p,"b"));
    const auto a=task_for(p,"a"),py=task_for(p,"python"),b=task_for(p,"b");CHECK(a<py);CHECK(py<b);
    CHECK(p.tasks[b].dependencies==std::vector<std::size_t>{a,py});
    std::size_t count=0;for(const auto& t:p.tasks)for(const auto& n:t.nodes)count+=n.node_id=="a";CHECK(count==1);
    // Two adjacent compatible nodes do form one region, not duplicate ready work.
    f.project.nodes.push_back({"extra","fixture.native",{}});f.project.connections[0].to_node="extra";
    f.project.connections.push_back({"extra","out","a","in"});
    outcome=compile_prepared_plan(f.project,f.catalog,{48000,2},513,512);REQUIRE(std::holds_alternative<PreparedPlan>(outcome));
    CHECK(task_for(std::get<PreparedPlan>(outcome),"extra")==task_for(std::get<PreparedPlan>(outcome),"a"));
}
TEST_CASE("P3-02 selection prunes offline tasks but retains global structural checks and format admission","[P3-02][pure]") {
    using namespace audioalgo;MixedFixture f;f.project.selected_output_node_id="a";f.project.selected_output_port_id="other";
    auto outcome=compile_prepared_plan(f.project,f.catalog,{48000,2},513,512);REQUIRE(std::holds_alternative<PreparedPlan>(outcome));
    const auto& p=std::get<PreparedPlan>(outcome);REQUIRE(p.tasks.size()==2);CHECK(p.terminal_node=="a");REQUIRE(p.selected_output);CHECK(p.selected_output->port_id=="other");
    f.project.connections.back().from_port="missing";outcome=compile_prepared_plan(f.project,f.catalog,{48000,2},513,512);
    REQUIRE(std::holds_alternative<Diagnostic>(outcome));CHECK(std::get<Diagnostic>(outcome).code=="connection_source_port_missing");
    MixedFixture bad;auto d=*bad.catalog.find("fixture.offline");d.type_id="fixture.invalid";d.audio_format.sample_rates_hz={16000};REQUIRE(bad.catalog.register_descriptor(d));bad.project.nodes[2].type_id=d.type_id;
    outcome=compile_prepared_plan(bad.project,bad.catalog,{48000,2},513,512);REQUIRE(std::holds_alternative<Diagnostic>(outcome));CHECK(std::get<Diagnostic>(outcome).node_id=="python");
}
