#include "audio_project/ProjectModel.h"
#include "plugins/builtin/registration.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using audioalgo::Connection;
using audioalgo::NodeInstance;
using audioalgo::NodeRegistry;
using audioalgo::ParameterValue;
using audioalgo::ProjectModel;
using audioalgo::register_builtin_nodes;
using audioalgo::topological_order;
using audioalgo::validate_project;

namespace {
NodeRegistry make_registry() {
    NodeRegistry registry;
    register_builtin_nodes(registry);
    return registry;
}

ProjectModel valid_gain_project() {
    ProjectModel project;
    project.project_id = "test-project";
    project.name = "Graph contract";
    project.nodes = {
        {"file", "builtin.file_source", {{"path", std::string("signal.wav")}}, 10.0, 10.0},
        {"gain", "builtin.gain", {{"gain_db", 0.0}}, 240.0, 10.0},
        {"out", "builtin.output", {}, 470.0, 10.0},
    };
    project.connections = {
        {"file", "audio", "gain", "audio_in"},
        {"gain", "audio_out", "out", "audio"},
    };
    project.selected_output_node_id = "out";
    return project;
}

bool has_code(const std::vector<audioalgo::GraphDiagnostic>& diagnostics, const std::string& code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) { return diagnostic.code == code; });
}
} // namespace

TEST_CASE("GRAPH-01 valid File to Gain to Output graphs topologically compile", "[G1][GRAPH-01]") {
    const auto registry = make_registry();
    auto project = valid_gain_project();
    REQUIRE(validate_project(project, registry).empty());
    std::vector<audioalgo::GraphDiagnostic> diagnostics;
    const auto order = topological_order(project, registry, diagnostics);
    REQUIRE(order.size() == 3);
    REQUIRE(order.front() == "file");
    REQUIRE(order.back() == "out");
}

TEST_CASE("GRAPH-01 rejects cycles, self edges, missing ports and duplicate required inputs", "[G1][GRAPH-01]") {
    const auto registry = make_registry();

    auto cyclic = valid_gain_project();
    cyclic.connections.push_back({"out", "audio", "file", "audio"});
    REQUIRE(has_code(validate_project(cyclic, registry), "connection_source_port_missing"));
    cyclic.connections.back() = {"gain", "audio_out", "gain", "audio_in"};
    const auto self_diagnostics = validate_project(cyclic, registry);
    REQUIRE(has_code(self_diagnostics, "connection_self"));

    cyclic.nodes = {
        {"gain1", "builtin.gain", {{"gain_db", 0.0}}, 20.0, 20.0},
        {"gain2", "builtin.gain", {{"gain_db", 0.0}}, 220.0, 20.0},
        {"out", "builtin.output", {}, 420.0, 20.0},
    };
    cyclic.connections = {
        {"gain1", "audio_out", "gain2", "audio_in"},
        {"gain2", "audio_out", "gain1", "audio_in"},
        {"gain1", "audio_out", "out", "audio"},
    };
    cyclic.selected_output_node_id = "out";
    const auto cycle_diagnostics = validate_project(cyclic, registry);
    REQUIRE(has_code(cycle_diagnostics, "graph_cycle"));

    auto missing = valid_gain_project();
    missing.connections.clear();
    const auto missing_diagnostics = validate_project(missing, registry);
    REQUIRE(has_code(missing_diagnostics, "input_required"));
    REQUIRE(std::any_of(missing_diagnostics.begin(), missing_diagnostics.end(), [](const auto& d) {
        return d.code == "input_required" && d.node_id == "gain" && d.port_id == "audio_in";
    }));

    auto duplicate = valid_gain_project();
    duplicate.nodes.push_back({"file2", "builtin.file_source", {{"path", std::string("other.wav")}}, 10.0, 180.0});
    duplicate.connections.push_back({"file2", "audio", "gain", "audio_in"});
    const auto duplicate_diagnostics = validate_project(duplicate, registry);
    REQUIRE(has_code(duplicate_diagnostics, "input_connection_duplicate"));
}

TEST_CASE("GRAPH-01 missing node and port diagnostics identify the endpoint", "[G1][GRAPH-01]") {
    const auto registry = make_registry();
    auto project = valid_gain_project();
    project.connections.front() = {"ghost", "audio", "gain", "audio_in"};
    const auto diagnostics = validate_project(project, registry);
    REQUIRE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& d) {
        return d.code == "connection_source_missing" && d.node_id == "ghost" && d.port_id == "audio";
    }));

    project = valid_gain_project();
    project.connections.front().from_port = "unknown";
    const auto port_diagnostics = validate_project(project, registry);
    REQUIRE(std::any_of(port_diagnostics.begin(), port_diagnostics.end(), [](const auto& d) {
        return d.code == "connection_source_port_missing" && d.node_id == "file" && d.port_id == "unknown";
    }));
}
