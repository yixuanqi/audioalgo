#include "audio_runtime/NativePlan.h"
#include <catch2/catch_test_macros.hpp>

namespace {
struct PlanFixture {
    audioalgo::NodeRegistry catalog;
    audioalgo::ProjectModel project;
    explicit PlanFixture(bool metric = false, std::size_t fixed_frames = 0, bool named = false) {
        using namespace audioalgo;
        REQUIRE(catalog.register_descriptor({"custom.source", "1.0.0", "Metadata source",
            {{"signal", "audio.pcm@1", PortDirection::output, false}}, {}, NodeExecutionKind::source}));
        NodeDescriptor kernel{"custom.kernel", "1.2.3", "Metadata kernel",
            {{"input-a", "audio.pcm@1", PortDirection::input, true},
             {"output-b", "audio.pcm@1", PortDirection::output, false}},
            {{"level", ParameterKind::floating, 0.5, 0.0, 2.0, "x"}}, NodeExecutionKind::block};
        if (metric)
            kernel.ports.push_back({"metric", "metrics.scalar@1", PortDirection::output, false});
        if (named) {
            kernel.capabilities.supports_named_audio_ports = true;
            kernel.ports.push_back({"secondary", "audio.pcm@1", PortDirection::input, true});
            kernel.ports.push_back({"alternate", "audio.pcm@1", PortDirection::output, false});
        }
        if (fixed_frames != 0) {
            kernel.capabilities.supports_variable_block_size = false;
            kernel.capabilities.fixed_block_frames = fixed_frames;
        }
        REQUIRE(catalog.register_descriptor(std::move(kernel)));
        REQUIRE(catalog.register_descriptor({"custom.sink", "1.0.0", "Metadata sink",
            {{"render", "audio.pcm@1", PortDirection::input, true}}, {}, NodeExecutionKind::sink}));
        project.project_id = "pure-plan";
        // Deliberately not in execution order. No descriptor has any registered factory.
        project.nodes = {{"output", "custom.sink", {}}, {"process", "custom.kernel", {}}, {"source", "custom.source", {}}};
        project.connections = {{"source", "signal", "process", "input-a"}, {"process", "output-b", "output", "render"}};
        project.selected_output_node_id = "output";
    }
};
}

TEST_CASE("AR-14.a pure native plan freezes task boundaries named routes and defaults", "[R5][AR-14.a][pure]") {
    PlanFixture fixture;
    const auto outcome = audioalgo::compile_native_plan(fixture.project, fixture.catalog, {48000, 2}, 129, 127);
    REQUIRE(std::holds_alternative<audioalgo::NativePlan>(outcome));
    const auto& plan = std::get<audioalgo::NativePlan>(outcome);
    REQUIRE(plan.tasks.size() == 3);
    CHECK(plan.tasks[0].kind == audioalgo::PlanTaskKind::source_boundary);
    CHECK(plan.tasks[0].node_ids == std::vector<std::string>{"source"});
    CHECK(plan.tasks[1].kind == audioalgo::PlanTaskKind::native_region);
    CHECK(plan.tasks[1].node_ids == std::vector<std::string>{"process"});
    CHECK(plan.tasks[2].kind == audioalgo::PlanTaskKind::sink_boundary);
    CHECK(plan.tasks[2].node_ids == std::vector<std::string>{"output"});
    REQUIRE(plan.nodes.size() == 3);
    CHECK(plan.nodes[0].node_id == "source");
    CHECK(plan.nodes[1].node_id == "process");
    CHECK(plan.nodes[2].node_id == "output");
    CHECK(plan.nodes[1].descriptor.version == "1.2.3");
    CHECK(std::get<double>(plan.nodes[1].parameters.at("level")) == 0.5);
    CHECK(fixture.project.nodes[1].parameters.empty());
    fixture.project.nodes[1].parameters["level"] = 1.5;
    CHECK(std::get<double>(plan.nodes[1].parameters.at("level")) == 0.5);
    CHECK(plan.format == (audioalgo::AudioFormat{48000, 2}));
    CHECK(plan.source_frames == 129);
    CHECK(plan.maximum_block_frames == 127);
    REQUIRE(plan.routes.size() == 2);
    CHECK(plan.routes[0].from.node_id == "source");
    CHECK(plan.routes[0].from.port_id == "signal");
    CHECK(plan.routes[0].to.node_id == "process");
    CHECK(plan.routes[0].to.port_id == "input-a");
    CHECK(plan.routes[1].from.port_id == "output-b");
    CHECK(plan.routes[1].to.port_id == "render");
    for (const auto& route : plan.routes) {
        CHECK(route.from_channel_base == 0);
        CHECK(route.to_channel_base == 0);
        CHECK(route.channel_count == 2);
    }
}

TEST_CASE("AR-15.e pure plan propagates selected port tails and exact downstream input ends","[R5][AR-15.e][pure][port-tail-plan]") {
    using namespace audioalgo;
    for(const std::string port:{"output-b","alternate"})for(const bool direct:{false,true})for(const bool stream:{false,true}) {
        PlanFixture fixture(false,480,true);auto d=*fixture.catalog.find("custom.kernel");d.type_id="custom.port_tails";
        d.capabilities.fixed_frame_end_policy=NodeExecutionCapabilities::FixedFrameEndPolicy::zero_pad;
        d.capabilities.tail_frames=5;d.capabilities.tail_processing=NodeExecutionCapabilities::TailProcessing::zero_padding;
        d.capabilities.maximum_zero_padding_frames=484;d.capabilities.output_tail_frames={{"output-b",2},{"alternate",5}};
        d.capabilities.uses_streaming_frame_api=stream;
        REQUIRE(fixture.catalog.register_descriptor(d));fixture.project.nodes[1].type_id=d.type_id;
        fixture.project.connections.push_back({"source","signal","process","secondary"});fixture.project.connections[1].from_port=port;
        if(direct){fixture.project.selected_output_node_id="process";fixture.project.selected_output_port_id=port;}
        const auto outcome=compile_native_plan(fixture.project,fixture.catalog,{48000,2},1025,127);
        REQUIRE(std::holds_alternative<NativePlan>(outcome));const auto& plan=std::get<NativePlan>(outcome);
        CHECK(plan.latency_frames==479);CHECK(plan.retained_tail_frames==(port=="output-b"?2:5));
        const auto& process=plan.nodes[1];CHECK(process.node_id=="process");CHECK(process.output_tail_frames==5);
        CHECK(process.output_tails.at("output-b")==2);CHECK(process.output_tails.at("alternate")==5);
        REQUIRE(process.input_ends.size()==2);CHECK(process.input_ends[0].frame==1025);CHECK(process.input_ends[1].frame==1025);
        if(!direct){REQUIRE(plan.nodes[2].input_ends.size()==1);CHECK(plan.nodes[2].input_ends[0].frame==(port=="output-b"?1506:1509));}
    }
}

TEST_CASE("AR-14.a pure plan refuses unsupported payloads and invalid model bindings", "[R5][AR-14.a][pure]") {
    {
        PlanFixture fixture(true);
        const auto outcome = audioalgo::compile_native_plan(fixture.project, fixture.catalog, {48000, 2}, 129, 127);
        const auto* error = std::get_if<audioalgo::Diagnostic>(&outcome);
        REQUIRE(error != nullptr);
        CHECK(error->code == "port_type_unsupported");
        CHECK(error->node_id == "process");
        CHECK(error->port_id == "metric");
    }
    {
        PlanFixture fixture;
        fixture.project.connections[1].from_port = "missing";
        const auto outcome = audioalgo::compile_native_plan(fixture.project, fixture.catalog, {48000, 2}, 129, 127);
        const auto* error = std::get_if<audioalgo::Diagnostic>(&outcome);
        REQUIRE(error != nullptr);
        CHECK(error->code == "connection_source_port_missing");
        CHECK(error->node_id == "process");
        CHECK(error->port_id == "missing");
    }
    {
        PlanFixture fixture;
        fixture.project.nodes[1].parameters["level"] = 2.5;
        const auto outcome = audioalgo::compile_native_plan(fixture.project, fixture.catalog, {48000, 2}, 129, 127);
        const auto* error = std::get_if<audioalgo::Diagnostic>(&outcome);
        REQUIRE(error != nullptr);
        CHECK(error->code == "parameter_out_of_range");
        CHECK(error->node_id == "process");
        CHECK(error->port_id == "level");
    }
}

TEST_CASE("AR-14.a pure plan preserves explicit fixed frame and tail admission", "[R5][AR-14.a][pure]") {
    PlanFixture fixture(false, 128);
    for (const auto [frames, host] : {std::pair<std::size_t, int>{256, 127}, {129, 128}}) {
        const auto outcome = audioalgo::compile_native_plan(fixture.project, fixture.catalog, {48000, 2}, frames, host);
        const auto* error = std::get_if<audioalgo::Diagnostic>(&outcome);
        REQUIRE(error != nullptr);
        CHECK(error->code == "capability_unsupported");
        CHECK(error->node_id == "process");
    }
    const auto outcome = audioalgo::compile_native_plan(fixture.project, fixture.catalog, {48000, 2}, 256, 128);
    REQUIRE(std::holds_alternative<audioalgo::NativePlan>(outcome));
    CHECK(std::get<audioalgo::NativePlan>(outcome).source_frames == 256);
    CHECK(std::get<audioalgo::NativePlan>(outcome).maximum_block_frames == 128);
}

TEST_CASE("AR-14.b pure plan keeps separate channel bases for named PCM groups", "[R5][AR-14.b][pure]") {
    PlanFixture fixture(false,0,true);
    fixture.project.connections[1].from_port="alternate";
    fixture.project.connections.push_back({"source","signal","process","secondary"});
    const auto outcome=audioalgo::compile_native_plan(fixture.project,fixture.catalog,{48000,2},129,127);
    REQUIRE(std::holds_alternative<audioalgo::NativePlan>(outcome));
    const auto& plan=std::get<audioalgo::NativePlan>(outcome);
    REQUIRE(plan.routes.size()==3);
    CHECK(plan.routes[1].from.port_id=="alternate");
    CHECK(plan.routes[1].from_channel_base==2);
    CHECK(plan.routes[1].to_channel_base==0);
    CHECK(plan.routes[2].to.port_id=="secondary");
    CHECK(plan.routes[2].from_channel_base==0);
    CHECK(plan.routes[2].to_channel_base==2);
    for (const auto& route:plan.routes) CHECK(route.channel_count==2);
}

TEST_CASE("AR-14.c pure plan prunes to the selected output closure", "[R5][AR-14.c][pure]") {
    // A metrics-output kernel hangs off the shared source but never reaches the selected output;
    // its unsupported payload must not block a pruned plan, and it must not appear in the plan.
    struct PruneFixture : PlanFixture {
        PruneFixture() : PlanFixture(false, 0, true) {
            using namespace audioalgo;
            REQUIRE(catalog.register_descriptor({"custom.metric_kernel", "1.0.0", "Metric kernel",
                {{"input-a", "audio.pcm@1", PortDirection::input, true},
                 {"metric", "metrics.scalar@1", PortDirection::output, false}},
                {}, NodeExecutionKind::block}));
            project.nodes.push_back({"spare", "custom.metric_kernel", {}});
            project.connections.push_back({"source", "signal", "process", "secondary"});
            project.connections.push_back({"source", "signal", "spare", "input-a"});
        }
    };

    SECTION("port-level selection compiles only the reverse closure") {
        PruneFixture fixture;
        fixture.project.selected_output_node_id = "process";
        fixture.project.selected_output_port_id = "alternate";
        const auto outcome = audioalgo::compile_native_plan(fixture.project, fixture.catalog, {48000, 2}, 129, 127);
        REQUIRE(std::holds_alternative<audioalgo::NativePlan>(outcome));
        const auto& plan = std::get<audioalgo::NativePlan>(outcome);
        REQUIRE(plan.selected_output.has_value());
        CHECK(plan.selected_output->node_id == "process");
        CHECK(plan.selected_output->port_id == "alternate");
        REQUIRE(plan.nodes.size() == 2);
        CHECK(plan.nodes[0].node_id == "source");
        CHECK(plan.nodes[1].node_id == "process");
        REQUIRE(plan.tasks.size() == 3);
        CHECK(plan.tasks[0].kind == audioalgo::PlanTaskKind::source_boundary);
        CHECK(plan.tasks[0].node_ids == std::vector<std::string>{"source"});
        CHECK(plan.tasks[1].kind == audioalgo::PlanTaskKind::native_region);
        CHECK(plan.tasks[1].node_ids == std::vector<std::string>{"process"});
        CHECK(plan.tasks[2].kind == audioalgo::PlanTaskKind::sink_boundary);
        CHECK(plan.tasks[2].node_ids == std::vector<std::string>{"process"});
        REQUIRE(plan.routes.size() == 2);
        CHECK(plan.routes[0].to.node_id == "process");
        CHECK(plan.routes[1].to.port_id == "secondary");
    }

    SECTION("legacy node-level selection prunes the same way") {
        PruneFixture fixture;
        const auto outcome = audioalgo::compile_native_plan(fixture.project, fixture.catalog, {48000, 2}, 129, 127);
        REQUIRE(std::holds_alternative<audioalgo::NativePlan>(outcome));
        const auto& plan = std::get<audioalgo::NativePlan>(outcome);
        CHECK_FALSE(plan.selected_output.has_value());
        REQUIRE(plan.nodes.size() == 3);
        CHECK(plan.nodes[0].node_id == "source");
        CHECK(plan.nodes[1].node_id == "process");
        CHECK(plan.nodes[2].node_id == "output");
        REQUIRE(plan.routes.size() == 3);
        for (const auto& route : plan.routes)
            CHECK(route.to.node_id != "spare");
    }

    SECTION("broken connections outside the closure are still rejected") {
        PruneFixture fixture;
        fixture.project.selected_output_node_id = "process";
        fixture.project.selected_output_port_id = "alternate";
        fixture.project.connections[3].from_port = "missing";
        const auto outcome = audioalgo::compile_native_plan(fixture.project, fixture.catalog, {48000, 2}, 129, 127);
        const auto* error = std::get_if<audioalgo::Diagnostic>(&outcome);
        REQUIRE(error != nullptr);
        CHECK(error->code == "connection_source_port_missing");
        CHECK(error->node_id == "source");
        CHECK(error->port_id == "missing");
    }
}
