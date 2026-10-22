#include "audio_io/WavFile.h"
#include "../../apps/editor/EditorGeometry.h"
#include "../../apps/editor/EditorNodeCreation.h"
#include "../../apps/editor/EditorTopologyCommands.h"
#include "audio_project/ProjectFileIO.h"
#include "audio_run_service/RunService.h"
#include "audio_editor_application/EditorController.h"
#include "../../apps/editor/EditorCompletionDispatch.h"
#include "../../apps/editor/EditorWorkerPool.h"
#include "../../apps/editor/EditorWorkJob.h"
#include "audio_editor_application/RunReception.h"
#include "plugins/builtin/registration.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "../fixtures/ParameterProbeNode.h"
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using audioalgo::AudioBuffer;
using audioalgo::NodeRegistry;
using audioalgo::ProjectModel;
using audioalgo::RunRequest;
using audioalgo::RunService;
using audioalgo::RunSuccess;
using audioalgo::WavEncoding;
using audioalgo::load_project_file;
using audioalgo::read_wav_file;
using audioalgo::register_builtin_nodes;
using audioalgo::save_project_file;
using audioalgo::write_wav_file;

namespace {
struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::current_path() / ".audioalgo-test-tmp" /
        std::filesystem::u8path("AudioAlgo 音频 E2E " + juce::Uuid().toString().toStdString());
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::vector<char> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

struct RunMetadata {
    std::string run_id;
    std::string project_id;
    std::uint64_t project_revision = 0;
    std::string input_file;
    std::string input_sha256;
    std::string output_sha256;
    std::string request_id;
    std::uint64_t session_epoch = 0;
};

std::optional<RunMetadata> read_run_metadata(const std::filesystem::path& path) {
    const auto path_utf8 = path_to_utf8(path);
    const auto json = juce::JSON::parse(
        juce::File(juce::String::fromUTF8(path_utf8.c_str())).loadFileAsString());
    const auto* object = json.getDynamicObject();
    if (object == nullptr)
        return std::nullopt;
    const auto run_id = object->getProperty("run_id");
    const auto project_id = object->getProperty("project_id");
    const auto revision = object->getProperty("project_revision");
    const auto input_file = object->getProperty("input_file");
    const auto input_hash = object->getProperty("input_sha256");
    const auto output_hash = object->getProperty("output_sha256");
    const auto request_id = object->getProperty("request_id");
    const auto session_epoch = object->getProperty("session_epoch");
    if (!run_id.isString() || !project_id.isString() || (!revision.isInt64() && !revision.isInt()) ||
        !input_file.isString() || !input_hash.isString() || !output_hash.isString() || !request_id.isString() ||
        (!session_epoch.isInt64() && !session_epoch.isInt()))
        return std::nullopt;
    return RunMetadata{
        run_id.toString().toStdString(), project_id.toString().toStdString(),
        static_cast<std::uint64_t>(static_cast<juce::int64>(revision)),
        input_file.toString().toStdString(),
        input_hash.toString().toStdString(), output_hash.toString().toStdString(),
        request_id.toString().toStdString(),
        static_cast<std::uint64_t>(static_cast<juce::int64>(session_epoch))};
}

struct CliRunOutcome {
    bool started = false;
    bool finished = false;
    int exit_code = -1;
    std::string output;
};

CliRunOutcome run_cli(const std::filesystem::path& project_path, const std::filesystem::path& output_directory) {
    const auto cli_executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
        .getSiblingFile("audio_cli.exe");
    const auto project_path_utf8 = path_to_utf8(project_path);
    const auto output_path_utf8 = path_to_utf8(output_directory);
    juce::StringArray arguments;
    arguments.add(cli_executable.getFullPathName());
    arguments.add("run");
    arguments.add(juce::String::fromUTF8(project_path_utf8.c_str()));
    arguments.add(juce::String::fromUTF8(output_path_utf8.c_str()));
    arguments.add("127");
    juce::ChildProcess child;
    const auto started = child.start(arguments);
    if (!started)
        return {false, false, -1, {}};
    const auto finished = child.waitForProcessToFinish(30000);
    return {true, finished, static_cast<int>(child.getExitCode()), child.readAllProcessOutput().toStdString()};
}

ProjectModel make_project() {
    ProjectModel project;
    project.project_id = "e2e-gain-project";
    project.name = "E2E Gain run";
    project.revision = 4;
    project.nodes = {
        {"file", "builtin.file_source", {{"path", std::string("input.wav")}}, 10.0, 30.0},
        {"gain", "builtin.gain", {{"gain_db", 6.0}}, 220.0, 30.0},
        {"output", "builtin.output", {}, 430.0, 30.0},
    };
    project.connections = {
        {"file", "audio", "gain", "audio_in"},
        {"gain", "audio_out", "output", "audio"},
    };
    project.selected_output_node_id = "output";
    return project;
}

struct RunDisplayState {
    std::string result = "B-result";
    std::string waveform = "B-waveform";
    std::filesystem::path playback_file = "B.wav";
    std::string status = "B-ready";
    bool operator==(const RunDisplayState&) const = default;
};
} // namespace

TEST_CASE("AR-08.g Controller and RunService reject schema violations with located diagnostics",
          "[R2][AR-08.g][native_integration]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "invalid-project.aaproj";
    {
        std::ofstream project_file(project_path, std::ios::binary);
        REQUIRE(project_file.good());
        project_file << "placeholder";
    }

    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    auto project = make_project();
    audioalgo::editor::EditorController controller(descriptors);
    controller.adopt_project(project);

    const auto controller_parameter = controller.set_parameter("gain", "gain_db", 99.0);
    REQUIRE(controller_parameter.status == audioalgo::editor::ParameterEditStatus::rejected);
    REQUIRE(controller_parameter.diagnostic.has_value());
    CHECK(controller_parameter.diagnostic->code == "parameter_out_of_range");
    CHECK(controller_parameter.diagnostic->node_id == "gain");
    CHECK(controller_parameter.diagnostic->port_id == "gain_db");

    const auto controller_connection = controller.connect({"gain", "missing_output", "output", "audio"});
    REQUIRE(controller_connection.status == audioalgo::editor::ProjectEditStatus::rejected);
    REQUIRE_FALSE(controller_connection.diagnostics.empty());
    CHECK(controller_connection.diagnostics.front().code == "connection_source_port_missing");
    CHECK(controller_connection.diagnostics.front().node_id == "gain");
    CHECK(controller_connection.diagnostics.front().port_id == "missing_output");

    RunService service(descriptors);
    auto invalid_parameter_project = project;
    invalid_parameter_project.nodes[1].parameters["gain_db"] = 99.0;
    const auto parameter_run = service.run({invalid_parameter_project, project_path,
                                            temporary.path / "invalid-parameter-run", 127});
    const auto* parameter_diagnostic = std::get_if<audioalgo::Diagnostic>(&parameter_run);
    REQUIRE(parameter_diagnostic != nullptr);
    CHECK(parameter_diagnostic->code == controller_parameter.diagnostic->code);
    CHECK(parameter_diagnostic->node_id == controller_parameter.diagnostic->node_id);
    CHECK(parameter_diagnostic->port_id == controller_parameter.diagnostic->port_id);
    CHECK(parameter_diagnostic->node_id == "gain");
    CHECK(parameter_diagnostic->port_id == "gain_db");

    auto invalid_connection_project = project;
    invalid_connection_project.connections.back().from_port = "missing_output";
    const auto connection_run = service.run({invalid_connection_project, project_path,
                                             temporary.path / "invalid-connection-run", 127});
    const auto* connection_diagnostic = std::get_if<audioalgo::Diagnostic>(&connection_run);
    REQUIRE(connection_diagnostic != nullptr);
    CHECK(connection_diagnostic->code == controller_connection.diagnostics.front().code);
    CHECK(connection_diagnostic->node_id == controller_connection.diagnostics.front().node_id);
    CHECK(connection_diagnostic->port_id == controller_connection.diagnostics.front().port_id);
    CHECK(connection_diagnostic->node_id == "gain");
    CHECK(connection_diagnostic->port_id == "missing_output");
}

TEST_CASE("AR-03.g", "[architecture][native_integration][run_identity]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "delayed A.aaproj";
    const auto input_path = temporary.path / "input.wav";
    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.3 * std::sin(frame * 0.03));
        input.mutable_channel(1)[frame] = static_cast<float>(0.2 * std::cos(frame * 0.05));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(write_wav_file(input_path, input, WavEncoding::float32)));
    auto project_a = make_project();
    project_a.project_id = "project-A";
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project_a)));

    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    RunService service(descriptors);
    const audioalgo::RunIdentity identity{"project-A", 11, "request-A", project_a.revision};
    RunRequest request{project_a, project_path, temporary.path / "A-run", 127};
    request.identity = identity; // assigned before the worker is queued

    audioalgo::RunReception reception;
    reception.begin(identity);
    std::mutex mutex;
    std::condition_variable condition;
    bool worker_started = false;
    bool release_worker = false;
    bool delivered = false;
    bool run_succeeded = false;
    bool identity_matches = false;
    std::string run_id;
    RunDisplayState display_b;
    const auto before_open = display_b;
    auto project_b_id = project_a.project_id;
    auto project_b_epoch = std::uint64_t{11};
    const auto project_b_revision = project_a.revision;

    std::thread worker([&] {
        {
            std::unique_lock lock(mutex);
            worker_started = true;
            condition.notify_one();
            condition.wait(lock, [&] { return release_worker; });
        }
        auto outcome = service.run(request);
        const auto* success = std::get_if<RunSuccess>(&outcome);
        run_succeeded = success != nullptr;
        if (success != nullptr) {
            run_id = success->run_id;
            identity_matches = success->identity == identity;
        }
        juce::MessageManager::callAsync([&, outcome = std::move(outcome)]() mutable {
            if (const auto* result = std::get_if<RunSuccess>(&outcome)) {
                if (reception.accepts(identity, project_b_id, project_b_epoch, project_b_revision))
                    display_b = {result->run_id, "A-waveform", result->output_file, "A-complete"};
            }
            {
                std::lock_guard lock(mutex);
                delivered = true;
            }
            condition.notify_one();
            juce::MessageManager::getInstance()->stopDispatchLoop();
        });
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, std::chrono::seconds(5), [&] { return worker_started; }));
    }

    // Opening a distinct project with the same revision revokes A's authority.
    reception.invalidate();
    project_b_id = "project-B";
    project_b_epoch = 12;
    const auto after_open = display_b;
    {
        std::lock_guard lock(mutex);
        release_worker = true;
    }
    condition.notify_one();
    auto* message_manager = juce::MessageManager::getInstance();
    std::thread watchdog([&] {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(5), [&] { return delivered; }))
            message_manager->stopDispatchLoop();
    });
    message_manager->runDispatchLoop();
    watchdog.join();
    worker.join();

    REQUIRE(run_succeeded);
    REQUIRE(identity_matches);
    REQUIRE_FALSE(run_id.empty());
    REQUIRE(run_id == identity.request_id);
    REQUIRE(delivered);
    REQUIRE(project_b_revision == project_a.revision);
    REQUIRE(display_b == after_open);
    REQUIRE(display_b == before_open);
    REQUIRE(std::filesystem::exists(temporary.path / "A-run" / "output.wav"));
    REQUIRE(reception.active() == std::nullopt);
    const auto metadata = read_run_metadata(temporary.path / "A-run" / "run.json");
    REQUIRE(metadata.has_value());
    REQUIRE(metadata->run_id == identity.request_id);
    REQUIRE(metadata->request_id == identity.request_id);
    REQUIRE(metadata->project_id == identity.project_id);
    REQUIRE(metadata->session_epoch == identity.session_epoch);
    REQUIRE(metadata->project_revision == identity.revision);
}

TEST_CASE("AR-09.d RunService keeps its registry alive across owner release",
          "[architecture][native_integration][lifecycle]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "registry lease.aaproj";
    const auto input_path = temporary.path / "input.wav";
    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.25 * std::sin(frame * 0.017));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.18 * std::cos(frame * 0.023));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));

    auto project = make_project();
    project.project_id = "registry-lease-project";
    CHECK(std::get<double>(project.nodes[1].parameters.at("gain_db")) == Catch::Approx(6.0));
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    auto registry = std::make_shared<NodeRegistry>();
    REQUIRE(register_builtin_nodes(*registry));
    std::weak_ptr<const NodeRegistry> registry_lifetime = registry;
    RunService service{std::shared_ptr<const NodeRegistry>(registry)};
    const audioalgo::RunIdentity identity{"registry-lease-project", 19, "registry-lease-run", project.revision};
    RunRequest request{project, project_path, temporary.path / "worker-run", 127};
    request.identity = identity;

    std::mutex mutex;
    std::condition_variable condition;
    bool worker_started = false;
    bool release_worker = false;
    bool worker_finished = false;
    std::optional<audioalgo::RunOutcome> outcome;
    auto* const message_manager = juce::MessageManager::getInstance();
    std::thread worker([service = std::move(service), request = std::move(request), registry_lifetime,
                        &mutex, &condition, &worker_started, &release_worker, &worker_finished, &outcome,
                        message_manager]() mutable {
        {
            std::unique_lock lock(mutex);
            worker_started = true;
            condition.notify_one();
            condition.wait(lock, [&] { return release_worker; });
        }
        if (registry_lifetime.expired()) {
            outcome = audioalgo::Diagnostic{"registry_released_early", "RunService lost its node registry"};
        } else {
            outcome = service.run(request);
        }
        {
            std::lock_guard lock(mutex);
            worker_finished = true;
        }
        condition.notify_one();
        message_manager->stopDispatchLoop();
    });

    bool started = false;
    {
        std::unique_lock lock(mutex);
        started = condition.wait_for(lock, std::chrono::seconds(5), [&] { return worker_started; });
    }
    registry.reset(); // Simulate the editor releasing its owner while the worker is still queued.
    const bool retained_after_owner_release = !registry_lifetime.expired();
    {
        std::lock_guard lock(mutex);
        release_worker = true;
    }
    condition.notify_one();
    message_manager->runDispatchLoop();
    bool finished = false;
    {
        std::lock_guard lock(mutex);
        finished = worker_finished;
    }
    worker.join();

    REQUIRE(started);
    REQUIRE(finished);
    REQUIRE(retained_after_owner_release);
    REQUIRE(outcome.has_value());
    REQUIRE(std::holds_alternative<RunSuccess>(*outcome));
    const auto& success = std::get<RunSuccess>(*outcome);
    CHECK(success.identity == identity);
    CHECK(success.run_id == identity.request_id);
    CHECK(success.frames == input.frame_count());
    CHECK(std::filesystem::exists(success.output_file));
    CHECK(std::filesystem::exists(success.manifest_file));
    CHECK(std::filesystem::exists(success.complete_file));
    const auto rendered = read_wav_file(success.output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(rendered));
    const auto& rendered_audio = std::get<AudioBuffer>(rendered);
    CHECK(rendered_audio.frame_count() == input.frame_count());
    CHECK(rendered_audio.sample_rate_hz() == input.sample_rate_hz());
    CHECK(rendered_audio.channel_count() == input.channel_count());
    const auto expected_gain = std::pow(10.0, 6.0 / 20.0);
    for (const auto frame : {std::size_t{1}, std::size_t{128}, std::size_t{512}}) {
        CHECK(rendered_audio.channel(0)[frame] == Catch::Approx(input.channel(0)[frame] * expected_gain).margin(2.0e-6));
        CHECK(rendered_audio.channel(1)[frame] == Catch::Approx(input.channel(1)[frame] * expected_gain).margin(2.0e-6));
    }
    const auto metadata = read_run_metadata(success.manifest_file);
    REQUIRE(metadata.has_value());
    CHECK(metadata->run_id == identity.request_id);
    CHECK(metadata->project_id == identity.project_id);
    CHECK(metadata->session_epoch == identity.session_epoch);
    CHECK(metadata->project_revision == identity.revision);
    CHECK(metadata->input_file == input_path.filename().string());
    CHECK(metadata->input_sha256 == success.input_sha256);
    CHECK(metadata->output_sha256 == success.output_sha256);

    if (const auto* artifact_path = std::getenv("AUDIOALGO_T05B_ARTIFACT_OUTPUT");
        artifact_path != nullptr && artifact_path[0] != '\0') {
        const auto artifact_directory = std::filesystem::u8path(artifact_path);
        REQUIRE_FALSE(std::filesystem::exists(artifact_directory));
        REQUIRE(std::filesystem::create_directories(artifact_directory));
        CHECK(std::filesystem::copy_file(success.output_file, artifact_directory / "output.wav"));
        CHECK(std::filesystem::copy_file(success.manifest_file, artifact_directory / "run.json"));
        CHECK(std::filesystem::copy_file(success.complete_file, artifact_directory / "complete.json"));
    }
    CHECK(registry_lifetime.expired());
}

TEST_CASE("AR-09.d queued completion is dropped after its component is destroyed",
          "[architecture][native_integration][lifecycle]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    struct CompletionTarget final : juce::Component {
        int delivery_count = 0;
        std::thread::id delivery_thread;
    };

    const auto message_thread = std::this_thread::get_id();
    auto live_target = std::make_unique<CompletionTarget>();
    auto destroyed_target = std::make_unique<CompletionTarget>();
    auto live_state = std::make_shared<audioalgo::editor::ComponentDispatchState<CompletionTarget>>(*live_target);
    auto destroyed_state = std::make_shared<audioalgo::editor::ComponentDispatchState<CompletionTarget>>(*destroyed_target);
    std::weak_ptr<audioalgo::editor::ComponentDispatchState<CompletionTarget>> live_pointer = live_state;
    std::weak_ptr<audioalgo::editor::ComponentDispatchState<CompletionTarget>> destroyed_pointer = destroyed_state;

    std::mutex mutex;
    std::condition_variable condition;
    bool callbacks_queued = false;
    bool live_scheduled = false;
    bool destroyed_scheduled = false;
    int destroyed_delivery_count = 0;
    std::thread worker([&] {
        live_scheduled = audioalgo::editor::dispatch_to_live_component(
            live_pointer, [&](CompletionTarget& target) {
                ++target.delivery_count;
                target.delivery_thread = std::this_thread::get_id();
            });
        destroyed_scheduled = audioalgo::editor::dispatch_to_live_component(
            destroyed_pointer, [&](CompletionTarget&) { ++destroyed_delivery_count; });
        {
            std::lock_guard lock(mutex);
            callbacks_queued = true;
        }
        condition.notify_one();
    });

    bool queued = false;
    {
        std::unique_lock lock(mutex);
        queued = condition.wait_for(lock, std::chrono::seconds(5), [&] { return callbacks_queued; });
    }
    worker.join();
    REQUIRE(queued);
    REQUIRE(live_scheduled);
    REQUIRE(destroyed_scheduled);

    destroyed_state->invalidate();
    destroyed_target.reset(); // The completion is queued, but its View is gone before delivery.
    destroyed_state.reset();
    const auto stop_posted = juce::MessageManager::callAsync([] {
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });
    REQUIRE(stop_posted);
    juce::MessageManager::getInstance()->runDispatchLoop();

    CHECK(live_target->delivery_count == 1);
    CHECK(live_target->delivery_thread == message_thread);
    CHECK(destroyed_delivery_count == 0);
    live_state->invalidate();
}

TEST_CASE("AR-09.e editor file work runs on a worker and completes on the message thread",
          "[architecture][native_integration][async_io]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    struct CompletionTarget final : juce::Component {
        bool completed = false;
        bool file_read_succeeded = false;
        std::thread::id work_thread;
        std::thread::id completion_thread;
    };

    CompletionTarget target;
    auto state = std::make_shared<audioalgo::editor::ComponentDispatchState<CompletionTarget>>(target);
    std::weak_ptr<audioalgo::editor::ComponentDispatchState<CompletionTarget>> weak_state = state;
    const auto message_thread = std::this_thread::get_id();
    const auto existing_source = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "apps" / "editor" / "Main.cpp";
    audioalgo::editor::EditorWorkerPool pool(1);
    auto* job = new audioalgo::editor::EditorWorkJob<CompletionTarget, std::pair<std::thread::id, bool>>(
        weak_state,
        [existing_source] {
            std::ifstream input(existing_source, std::ios::binary);
            char first_byte = 0;
            input.read(&first_byte, 1);
            return std::pair{std::this_thread::get_id(), input.gcount() == 1};
        },
        [](CompletionTarget& component, std::pair<std::thread::id, bool> result) {
            component.work_thread = result.first;
            component.file_read_succeeded = result.second;
            component.completion_thread = std::this_thread::get_id();
            component.completed = true;
            juce::MessageManager::getInstance()->stopDispatchLoop();
        });
    REQUIRE(pool.add_job(job, true));

    std::mutex watchdog_mutex;
    std::condition_variable watchdog_condition;
    bool dispatch_finished = false;
    std::thread watchdog([&] {
        std::unique_lock lock(watchdog_mutex);
        if (!watchdog_condition.wait_for(lock, std::chrono::seconds(5), [&] { return dispatch_finished; })) {
            lock.unlock();
            (void)juce::MessageManager::callAsync([] {
                juce::MessageManager::getInstance()->stopDispatchLoop();
            });
        }
    });

    juce::MessageManager::getInstance()->runDispatchLoop();
    {
        std::lock_guard lock(watchdog_mutex);
        dispatch_finished = true;
    }
    watchdog_condition.notify_one();
    watchdog.join();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pool.get_num_jobs() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    CHECK(target.completed);
    CHECK(target.file_read_succeeded);
    CHECK(target.work_thread != message_thread);
    CHECK(target.completion_thread == message_thread);
    state->invalidate();
}

TEST_CASE("PERF-01 editor heartbeat stays responsive during a controlled slow Python task",
          "[architecture][native_integration][performance][PERF-01]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    struct WorkObservation {
        std::thread::id work_thread;
        bool file_read_succeeded = false;
        bool python_started = false;
        bool python_finished = false;
    };
    struct CompletionTarget final : juce::Component {
        bool completed = false;
        WorkObservation observation;
        std::thread::id completion_thread;
    };
    struct Heartbeat final : juce::Timer {
        explicit Heartbeat(std::chrono::steady_clock::time_point started)
            : started(started), previous(started) {}

        void timerCallback() override {
            const auto now = std::chrono::steady_clock::now();
            gaps_ms.push_back(std::chrono::duration<double, std::milli>(now - previous).count());
            previous = now;
        }

        std::chrono::steady_clock::time_point started;
        std::chrono::steady_clock::time_point previous;
        std::vector<double> gaps_ms;
    };

    CompletionTarget target;
    auto state = std::make_shared<audioalgo::editor::ComponentDispatchState<CompletionTarget>>(target);
    std::weak_ptr<audioalgo::editor::ComponentDispatchState<CompletionTarget>> weak_state = state;
    const auto message_thread = std::this_thread::get_id();
    const auto existing_source = std::filesystem::path(AUDIOALGO_SOURCE_DIR) / "apps" / "editor" / "Main.cpp";
    audioalgo::editor::EditorWorkerPool pool(1);
    auto* job = new audioalgo::editor::EditorWorkJob<CompletionTarget, WorkObservation>(
        weak_state,
        [existing_source] {
            std::ifstream input(existing_source, std::ios::binary);
            char first_byte = 0;
            input.read(&first_byte, 1);
            const bool file_read_succeeded = input.gcount() == 1;
            juce::StringArray arguments;
            arguments.add(juce::String::fromUTF8(AUDIOALGO_PYTHON_EXECUTABLE));
            arguments.add("-c");
            arguments.add("import time; time.sleep(10)");
            juce::ChildProcess child;
            const bool python_started = child.start(arguments);
            const bool process_finished = python_started && child.waitForProcessToFinish(12000);
            return WorkObservation{std::this_thread::get_id(), file_read_succeeded, python_started,
                                   process_finished && child.getExitCode() == 0};
        },
        [](CompletionTarget& component, WorkObservation observation) {
            component.observation = observation;
            component.completion_thread = std::this_thread::get_id();
            component.completed = true;
            juce::MessageManager::getInstance()->stopDispatchLoop();
        });
    REQUIRE(pool.add_job(job, true));

    const auto started = std::chrono::steady_clock::now();
    Heartbeat heartbeat(started);
    heartbeat.startTimer(20);
    std::mutex watchdog_mutex;
    std::condition_variable watchdog_condition;
    bool dispatch_finished = false;
    std::thread watchdog([&] {
        std::unique_lock lock(watchdog_mutex);
        if (!watchdog_condition.wait_for(lock, std::chrono::seconds(15), [&] { return dispatch_finished; })) {
            lock.unlock();
            (void)juce::MessageManager::callAsync([] {
                juce::MessageManager::getInstance()->stopDispatchLoop();
            });
        }
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    {
        std::lock_guard lock(watchdog_mutex);
        dispatch_finished = true;
    }
    watchdog_condition.notify_one();
    watchdog.join();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pool.get_num_jobs() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    std::sort(heartbeat.gaps_ms.begin(), heartbeat.gaps_ms.end());
    const auto p95_index = heartbeat.gaps_ms.empty() ? 0 :
        static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(heartbeat.gaps_ms.size()))) - 1;
    const auto p95_ms = heartbeat.gaps_ms.empty() ? 0.0 : heartbeat.gaps_ms[p95_index];
    const auto maximum_ms = heartbeat.gaps_ms.empty() ? 0.0 : heartbeat.gaps_ms.back();
    std::cout << "PERF-01 samples=" << heartbeat.gaps_ms.size()
              << " p95_heartbeat_interval_ms=" << p95_ms
              << " max_heartbeat_interval_ms=" << maximum_ms << '\n';

    CHECK(std::chrono::steady_clock::now() - started >= std::chrono::seconds(10));
    CHECK(heartbeat.gaps_ms.size() >= 450);
    CHECK(p95_ms <= 100.0);
    CHECK(maximum_ms <= 500.0);
    CHECK(target.completed);
    CHECK(target.observation.file_read_succeeded);
    CHECK(target.observation.python_started);
    CHECK(target.observation.python_finished);
    CHECK(target.observation.work_thread != message_thread);
    CHECK(target.completion_thread == message_thread);
    state->invalidate();
}

TEST_CASE("AR-09.d editor shutdown drains a timed-out worker before destroying its view",
          "[architecture][native_integration][lifecycle]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    struct Gate {
        std::mutex mutex;
        std::condition_variable condition;
        bool entered = false;
        bool release = false;
        bool message_callback_delivered = false;
    };
    struct CompletionTarget final : juce::Component {
        std::atomic<int> delivery_count{0};
    };
    struct ControlledRunJob final : juce::ThreadPoolJob {
        ControlledRunJob(std::shared_ptr<Gate> gate,
                         std::shared_ptr<audioalgo::NodeRegistry> registry,
                         std::weak_ptr<audioalgo::editor::ComponentDispatchState<CompletionTarget>> target,
                         std::atomic<bool>& destroyed,
                         std::atomic<bool>& exit_requested,
                         std::atomic<bool>& completion_posted,
                         std::atomic<bool>& message_dispatch_posted,
                         std::atomic<bool>& message_pump_acknowledged)
            : ThreadPoolJob("controlled editor render"), gate(std::move(gate)), registry(std::move(registry)),
              target(std::move(target)), destroyed(destroyed), exit_requested(exit_requested),
              completion_posted(completion_posted), message_dispatch_posted(message_dispatch_posted),
              message_pump_acknowledged(message_pump_acknowledged) {}

        ~ControlledRunJob() override { destroyed.store(true); }

        JobStatus runJob() override {
            {
                std::unique_lock lock(gate->mutex);
                gate->entered = true;
                gate->condition.notify_one();
                gate->condition.wait(lock, [&] { return gate->release; });
            }
            exit_requested.store(shouldExit());
            message_dispatch_posted.store(juce::MessageManager::callAsync([gate = gate] {
                {
                    std::lock_guard lock(gate->mutex);
                    gate->message_callback_delivered = true;
                }
                gate->condition.notify_one();
            }));
            if (message_dispatch_posted.load()) {
                std::unique_lock lock(gate->mutex);
                message_pump_acknowledged.store(gate->condition.wait_for(
                    lock, std::chrono::seconds(5), [&] { return gate->message_callback_delivered; }));
            }
            completion_posted.store(audioalgo::editor::dispatch_to_live_component(
                target, [](CompletionTarget& component) { ++component.delivery_count; }));
            return jobHasFinished;
        }

        std::shared_ptr<Gate> gate;
        std::shared_ptr<audioalgo::NodeRegistry> registry;
        std::weak_ptr<audioalgo::editor::ComponentDispatchState<CompletionTarget>> target;
        std::atomic<bool>& destroyed;
        std::atomic<bool>& exit_requested;
        std::atomic<bool>& completion_posted;
        std::atomic<bool>& message_dispatch_posted;
        std::atomic<bool>& message_pump_acknowledged;
    };
    struct QueuedJob final : juce::ThreadPoolJob {
        QueuedJob(std::atomic<bool>& destroyed, std::atomic<bool>& ran)
            : ThreadPoolJob("queued editor render"), destroyed(destroyed), ran(ran) {}
        ~QueuedJob() override { destroyed.store(true); }
        JobStatus runJob() override { ran.store(true); return jobHasFinished; }
        std::atomic<bool>& destroyed;
        std::atomic<bool>& ran;
    };

    auto gate = std::make_shared<Gate>();
    auto target = std::make_unique<CompletionTarget>();
    auto lifetime_state = std::make_shared<
        audioalgo::editor::ComponentDispatchState<CompletionTarget>>(*target);
    std::weak_ptr<audioalgo::editor::ComponentDispatchState<CompletionTarget>> weak_target = lifetime_state;
    std::atomic<bool> active_job_destroyed{false};
    std::atomic<bool> active_job_observed_cancel{false};
    std::atomic<bool> active_job_posted_completion{false};
    std::atomic<bool> message_dispatch_posted{false};
    std::atomic<bool> message_pump_acknowledged{false};
    std::atomic<bool> queued_job_destroyed{false};
    std::atomic<bool> queued_job_ran{false};
    std::weak_ptr<audioalgo::NodeRegistry> registry_lifetime;
    bool registry_retained_while_running = false;
    bool shutdown_started = false;
    bool shutdown_timeout_observed = false;
    bool shutdown_callback_on_message_thread = false;
    bool pool_empty_at_shutdown_callback = false;
    bool worker_pool_destroyed = false;
    const auto message_thread = std::this_thread::get_id();

    {
        audioalgo::editor::EditorWorkerPool worker_pool(1);
        auto registry = std::make_shared<audioalgo::NodeRegistry>();
        registry_lifetime = registry;
        worker_pool.add_job(new ControlledRunJob(
            gate, registry, weak_target, active_job_destroyed,
            active_job_observed_cancel, active_job_posted_completion,
            message_dispatch_posted, message_pump_acknowledged), true);
        bool active_job_entered = false;
        {
            std::unique_lock lock(gate->mutex);
            gate->condition.wait_for(lock, std::chrono::seconds(5), [&] { return gate->entered; });
            active_job_entered = gate->entered;
        }
        worker_pool.add_job(new QueuedJob(queued_job_destroyed, queued_job_ran), true);
        registry.reset();
        registry_retained_while_running = active_job_entered && !registry_lifetime.expired();

        // This is the same first action as EditorComponent's close path.
        // The worker's eventual completion must not reach the closing View.
        lifetime_state->invalidate();

        std::thread release_job([gate] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5200));
            {
                std::lock_guard lock(gate->mutex);
                gate->release = true;
            }
            gate->condition.notify_one();
        });

        shutdown_started = worker_pool.shutdown_async(5000, [&](bool first_wait_timed_out) {
            shutdown_timeout_observed = first_wait_timed_out;
            shutdown_callback_on_message_thread = std::this_thread::get_id() == message_thread;
            pool_empty_at_shutdown_callback = worker_pool.get_num_jobs() == 0;
            juce::MessageManager::getInstance()->stopDispatchLoop();
        });
        if (!shutdown_started)
            juce::MessageManager::callAsync([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
        juce::MessageManager::getInstance()->runDispatchLoop();
        release_job.join();
        worker_pool.join_shutdown();
    }
    worker_pool_destroyed = true;

    CHECK(registry_retained_while_running);
    CHECK(shutdown_started);
    CHECK(shutdown_timeout_observed);
    CHECK(shutdown_callback_on_message_thread);
    CHECK(pool_empty_at_shutdown_callback);
    CHECK(active_job_observed_cancel.load());
    CHECK(message_dispatch_posted.load());
    CHECK(message_pump_acknowledged.load());
    CHECK(active_job_destroyed.load());
    CHECK(queued_job_destroyed.load());
    CHECK_FALSE(queued_job_ran.load());
    CHECK_FALSE(active_job_posted_completion.load());
    CHECK(target->delivery_count.load() == 0);
    CHECK(registry_lifetime.expired());
    CHECK(worker_pool_destroyed);
}

TEST_CASE("E2E-01 RunService and CLI share a project snapshot and gain oracle", "[G1][E2E-01]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "gain project.aaproj";
    const auto input_path = temporary.path / "input.wav";

    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.4 * std::sin(frame * 0.031));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.2 * std::cos(frame * 0.047));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(write_wav_file(input_path, input, WavEncoding::float32)));
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, make_project())));

    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    RunService service(descriptors);

    const auto service_output_dir = temporary.path / "service-run";
    const auto service_run = service.run({std::get<ProjectModel>(loaded), project_path, service_output_dir, 127});
    REQUIRE(std::holds_alternative<RunSuccess>(service_run));
    const auto& service_result = std::get<RunSuccess>(service_run);
    REQUIRE(std::filesystem::exists(service_result.complete_file));
    REQUIRE(std::filesystem::exists(service_result.manifest_file));
    REQUIRE(service_result.frames == input.frame_count());
    REQUIRE(service_result.input_sha256.size() == 64);
    REQUIRE(service_result.output_sha256.size() == 64);

    const auto service_audio = read_wav_file(service_result.output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(service_audio));
    const auto& rendered = std::get<AudioBuffer>(service_audio);
    const auto expected_gain = std::pow(10.0, 6.0 / 20.0);
    REQUIRE(rendered.channel_count() == input.channel_count());
    REQUIRE(rendered.frame_count() == input.frame_count());
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame)
            REQUIRE(rendered.channel(channel)[frame] == Catch::Approx(input.channel(channel)[frame] * expected_gain).margin(2.0e-6));
    }

    const auto cli_output_dir = temporary.path / "cli-run";
    const auto cli_executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");
    const auto project_path_arg = path_to_utf8(project_path);
    const auto output_path_arg = path_to_utf8(cli_output_dir);
    juce::StringArray arguments;
    arguments.add(cli_executable.getFullPathName());
    arguments.add("run");
    arguments.add(juce::String::fromUTF8(project_path_arg.c_str()));
    arguments.add(juce::String::fromUTF8(output_path_arg.c_str()));
    arguments.add("127");
    juce::ChildProcess child;
    REQUIRE(child.start(arguments));
    REQUIRE(child.waitForProcessToFinish(30000));
    const auto cli_stdout = child.readAllProcessOutput();
    INFO(cli_stdout.toStdString());
    REQUIRE(child.getExitCode() == 0);
    REQUIRE(std::filesystem::exists(cli_output_dir / "complete.json"));

    const auto cli_audio = read_wav_file(cli_output_dir / "output.wav");
    REQUIRE(std::holds_alternative<AudioBuffer>(cli_audio));
    const auto& cli_rendered = std::get<AudioBuffer>(cli_audio);
    REQUIRE(cli_rendered.frame_count() == rendered.frame_count());
    for (std::size_t channel = 0; channel < rendered.channel_count(); ++channel)
        REQUIRE(std::equal(rendered.channel(channel).begin(), rendered.channel(channel).end(), cli_rendered.channel(channel).begin()));
}

// AR-14.c: only sources inside the selected output's reverse closure are materialized. The
// unselected branch's source file never exists on disk, proving it is never read or captured.
TEST_CASE("AR-14.c run service materializes only closure sources for a port selection", "[R5][AR-14.c]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    AudioBuffer a(48000, {"left", "right"}, 300);
    for (std::size_t frame = 0; frame < 300; ++frame) {
        a.mutable_channel(0)[frame] = static_cast<float>(0.3 * std::sin(frame * 0.05));
        a.mutable_channel(1)[frame] = static_cast<float>(-0.2 * std::cos(frame * 0.04));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(write_wav_file(temporary.path / "a.wav", a, WavEncoding::float32)));
    const auto project_path = temporary.path / "pruned sources.aaproj";
    ProjectModel project;
    project.project_id = "ar14c-pruned-sources";
    project.selected_output_node_id = "mix";
    project.selected_output_port_id = "sum";
    project.nodes = {{"a", "builtin.file_source", {{"path", std::string("a.wav")}}},
                     // "pruned.wav" is deliberately never created: the unselected branch's source
                     // must not be resolved or captured at all.
                     {"pruned", "builtin.file_source", {{"path", std::string("pruned.wav")}}},
                     {"mix", "builtin.mixer", {}},
                     {"gain", "builtin.gain", {}},
                     {"out2", "builtin.output", {}}};
    project.connections = {{"a", "audio", "mix", "primary"},
                           {"a", "audio", "mix", "secondary"},
                           {"pruned", "audio", "gain", "audio_in"},
                           {"gain", "audio_out", "out2", "audio"}};
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    RunService service(descriptors);
    const auto run = service.run({project, project_path, temporary.path / "run", 64});
    const auto* error = std::get_if<audioalgo::Diagnostic>(&run);
    INFO("run error: " << (error != nullptr ? error->code + ": " + error->message : std::string("none")));
    REQUIRE(error == nullptr);
    const auto& result = std::get<RunSuccess>(run);
    REQUIRE(result.frames == 300);

    // Exactly one captured resource: the closure's source. The unselected source is absent.
    const auto manifest = juce::File(path_to_utf8(temporary.path / "run" / "run.json"));
    REQUIRE(manifest.existsAsFile());
    const auto document = juce::JSON::parse(manifest.loadFileAsString());
    const auto* resources = document.getProperty("resources", {}).getArray();
    REQUIRE(resources != nullptr);
    REQUIRE(resources->size() == 1);
    CHECK(resources->getFirst().getProperty("node_id", {}).toString() == "a");

    const auto audio = read_wav_file(result.output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(audio));
    const auto& rendered = std::get<AudioBuffer>(audio);
    for (std::size_t channel = 0; channel < 2; ++channel)
        for (std::size_t frame = 0; frame < 300; ++frame)
            // Default mixer gains are 0 dB: primary and secondary both carry a, so sum = 2a.
            REQUIRE(rendered.channel(channel)[frame] == Catch::Approx(2.0 * a.channel(channel)[frame]).margin(1.0e-6));
}

TEST_CASE("GRAPH-04 default CLI composition runs and replays a real dual-source mixer project", "[G2][GRAPH-04]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "dual source mixer.aaproj";
    AudioBuffer a(48000, {"left", "right"}, 513), b(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < 513; ++frame) {
        a.mutable_channel(0)[frame] = static_cast<float>(0.35 * std::sin(frame * 0.021));
        a.mutable_channel(1)[frame] = static_cast<float>(0.15 * std::cos(frame * 0.017));
        b.mutable_channel(0)[frame] = static_cast<float>(-0.25 * std::cos(frame * 0.013));
        b.mutable_channel(1)[frame] = static_cast<float>(0.45 * std::sin(frame * 0.029));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(write_wav_file(temporary.path / "a.wav", a, WavEncoding::float32)));
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(write_wav_file(temporary.path / "b.wav", b, WavEncoding::float32)));
    ProjectModel project;
    project.project_id = "graph04-cli-mixer";
    project.selected_output_node_id = "output";
    project.nodes = {{"a", "builtin.file_source", {{"path", std::string("a.wav")}}},
                     {"b", "builtin.file_source", {{"path", std::string("b.wav")}}},
                     {"mix", "builtin.mixer", {{"primary_gain_db", -6.0}, {"secondary_gain_db", 6.0}}},
                     {"output", "builtin.output", {}}};
    project.connections = {{"a", "audio", "mix", "primary"}, {"b", "audio", "mix", "secondary"},
                           {"mix", "sum", "output", "audio"}};
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    RunService service(descriptors);
    const auto service_output_dir = temporary.path / "service-run";
    const auto service_run = service.run({project, project_path, service_output_dir, 127});
    const auto* service_error = std::get_if<audioalgo::Diagnostic>(&service_run);
    INFO("service error: " << (service_error != nullptr ? service_error->code + ": " + service_error->message : std::string("none")));
    REQUIRE(service_error == nullptr);
    const auto& service_result = std::get<RunSuccess>(service_run);
    REQUIRE(service_result.frames == 513);

    const double primary_weight = std::pow(10.0, -6.0 / 20.0), secondary_weight = std::pow(10.0, 6.0 / 20.0);
    const auto service_audio = read_wav_file(service_result.output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(service_audio));
    const auto& rendered = std::get<AudioBuffer>(service_audio);
    for (std::size_t channel = 0; channel < 2; ++channel)
        for (std::size_t frame = 0; frame < 513; ++frame)
            REQUIRE(rendered.channel(channel)[frame] == Catch::Approx(
                primary_weight * a.channel(channel)[frame] + secondary_weight * b.channel(channel)[frame]).margin(1.0e-6));

    // The default composition root (CLI) must load and run the very same project.
    const auto cli_output_dir = temporary.path / "cli-run";
    const auto cli_executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");
    juce::StringArray arguments;
    arguments.add(cli_executable.getFullPathName());
    arguments.add("run");
    arguments.add(juce::String::fromUTF8(path_to_utf8(project_path).c_str()));
    arguments.add(juce::String::fromUTF8(path_to_utf8(cli_output_dir).c_str()));
    arguments.add("127");
    juce::ChildProcess child;
    REQUIRE(child.start(arguments));
    REQUIRE(child.waitForProcessToFinish(30000));
    const auto cli_stdout = child.readAllProcessOutput();
    INFO(cli_stdout.toStdString());
    REQUIRE(child.getExitCode() == 0);
    REQUIRE(std::filesystem::exists(cli_output_dir / "complete.json"));
    const auto cli_audio = read_wav_file(cli_output_dir / "output.wav");
    REQUIRE(std::holds_alternative<AudioBuffer>(cli_audio));
    const auto& cli_rendered = std::get<AudioBuffer>(cli_audio);
    for (std::size_t channel = 0; channel < 2; ++channel)
        REQUIRE(std::equal(rendered.channel(channel).begin(), rendered.channel(channel).end(), cli_rendered.channel(channel).begin()));

    // Persist artifacts for the independent Python oracle when requested. The originals must be
    // exported before the replay step deletes them.
    std::filesystem::path oracle_root;
    if (const auto* location = std::getenv("AUDIOALGO_MIXER_ARTIFACT_OUTPUT"); location && *location) {
        oracle_root = std::filesystem::u8path(location);
        REQUIRE_FALSE(std::filesystem::exists(oracle_root));
        std::filesystem::create_directories(oracle_root);
        for (const auto* file : {"a.wav", "b.wav"})
            REQUIRE(std::filesystem::copy_file(temporary.path / file, oracle_root / file));
        REQUIRE(std::filesystem::copy_file(project_path, oracle_root / "project.aaproj"));
        std::filesystem::copy(service_output_dir, oracle_root / "service-run",
                              std::filesystem::copy_options::recursive);
        std::filesystem::copy(cli_output_dir, oracle_root / "cli-run",
                              std::filesystem::copy_options::recursive);
    }

    // Replay from the CLI snapshot works after the caller's originals are gone.
    for (const auto* file : {"a.wav", "b.wav"})
        REQUIRE(std::filesystem::remove(temporary.path / file));
    const auto replay_project = load_project_file(cli_output_dir / "replay.aaproj");
    REQUIRE(std::holds_alternative<ProjectModel>(replay_project));
    const auto replay_run = service.run({std::get<ProjectModel>(replay_project), cli_output_dir / "replay.aaproj",
                                         temporary.path / "replay-run", 127});
    const auto* replay_error = std::get_if<audioalgo::Diagnostic>(&replay_run);
    INFO("replay error: " << (replay_error != nullptr ? replay_error->code + ": " + replay_error->message : std::string("none")));
    REQUIRE(replay_error == nullptr);
    const auto replay_audio = read_wav_file(std::get<RunSuccess>(replay_run).output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(replay_audio));
    const auto& replay_rendered = std::get<AudioBuffer>(replay_audio);
    for (std::size_t channel = 0; channel < 2; ++channel)
        REQUIRE(std::equal(rendered.channel(channel).begin(), rendered.channel(channel).end(), replay_rendered.channel(channel).begin()));
    if (!oracle_root.empty())
        std::filesystem::copy(temporary.path / "replay-run", oracle_root / "replay-run",
                              std::filesystem::copy_options::recursive);
}

TEST_CASE("NED-04.c", "[node_interaction][NED-04]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    auto project = make_project();
    project.connections.clear();
    std::get<std::string>(project.nodes.front().parameters.at("path")) = "missing.wav";
    const auto project_path = temporary.path / "incomplete.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    RunService service(descriptors);

    const auto service_output = temporary.path / "service-run";
    const auto service_result = service.run({project, project_path, service_output, 127});
    const auto* service_error = std::get_if<audioalgo::Diagnostic>(&service_result);
    REQUIRE(service_error != nullptr);
    REQUIRE(service_error->code == "input_required");
    REQUIRE_FALSE(std::filesystem::exists(service_output));

    const auto cli_output = temporary.path / "cli-run";
    const auto cli_executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("audio_cli.exe");
    juce::StringArray arguments;
    arguments.add(cli_executable.getFullPathName());
    arguments.add("run");
    arguments.add(juce::String::fromUTF8(path_to_utf8(project_path).c_str()));
    arguments.add(juce::String::fromUTF8(path_to_utf8(cli_output).c_str()));
    arguments.add("127");
    juce::ChildProcess child;
    REQUIRE(child.start(arguments));
    REQUIRE(child.waitForProcessToFinish(30000));
    const auto cli_output_text = child.readAllProcessOutput();
    INFO(cli_output_text.toStdString());
    REQUIRE(child.getExitCode() == 1);
    REQUIRE(cli_output_text.contains("input_required"));
    REQUIRE_FALSE(std::filesystem::exists(cli_output / "complete.json"));
}

TEST_CASE("NED-10.g", "[node_interaction][NED-10]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;

    auto project = make_project();
    project.revision = 40;
    const audioalgo::Connection target{"file", "audio", "gain", "audio_in"};

    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    REQUIRE(descriptors.register_descriptor({
        "test.required_side_input", "1.0.0", "Required side input block",
        {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"control_in", "control.value@1", audioalgo::PortDirection::input, true},
         {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}}, {},
        audioalgo::NodeExecutionKind::block}));

    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "test.required_side_input", "sidechain", target, 310.0, 30.0);
    REQUIRE(inserted.has_value());
    REQUIRE(inserted->input_port_id == "audio_in");
    REQUIRE(inserted->output_port_id == "audio_out");
    REQUIRE(project.nodes.size() == 4);
    REQUIRE(project.connections.size() == 3);
    const auto has_connection = [&](const audioalgo::Connection& expected) {
        return std::count_if(project.connections.begin(), project.connections.end(), [&](const auto& actual) {
            return actual.from_node == expected.from_node && actual.from_port == expected.from_port &&
                   actual.to_node == expected.to_node && actual.to_port == expected.to_port;
        }) == 1;
    };
    REQUIRE(has_connection({"file", "audio", "sidechain", "audio_in"}));
    REQUIRE(has_connection({"sidechain", "audio_out", "gain", "audio_in"}));
    REQUIRE(has_connection({"gain", "audio_out", "output", "audio"}));
    REQUIRE(project.revision == 41);

    const auto edit_diagnostics = audioalgo::validate_project_structure(project, descriptors);
    REQUIRE(edit_diagnostics.empty());
    const auto run_diagnostics = audioalgo::validate_project(project, descriptors);
    const auto missing_side_input = std::find_if(run_diagnostics.begin(), run_diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "input_required" && diagnostic.node_id == "sidechain" &&
               diagnostic.port_id == "control_in";
    });
    REQUIRE(missing_side_input != run_diagnostics.end());

    const auto project_path = temporary.path / "inserted-incomplete.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    RunService service(descriptors);
    const auto output_directory = temporary.path / "incomplete-run";
    const auto run_result = service.run({project, project_path, output_directory, 127});
    const auto* run_error = std::get_if<audioalgo::Diagnostic>(&run_result);
    REQUIRE(run_error != nullptr);
    REQUIRE(run_error->code == "input_required");
    REQUIRE_FALSE(std::filesystem::exists(output_directory));
}

TEST_CASE("NED-06.d", "[node_interaction][NED-06]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "layout-only.aaproj";
    const auto input_path = temporary.path / "input.wav";
    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.35 * std::sin(frame * 0.037));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.18 * std::cos(frame * 0.051));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(write_wav_file(input_path, input, WavEncoding::float32)));

    auto project = make_project();
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    const auto original_revision = project.revision;

    NodeRegistry geometry_registry;
    register_builtin_nodes(geometry_registry);
    const auto preview_transform = audioalgo::editor::CanvasCoordinateTransform{};
    auto preview_node = project.nodes[1];
    preview_node.x = 220.0;
    preview_node.y = 180.0;
    const auto preview_geometry = audioalgo::editor::make_node_geometry(
        preview_node, geometry_registry.find(preview_node.type_id), preview_transform);
    const auto preview_input = std::find_if(preview_geometry.ports.begin(), preview_geometry.ports.end(),
        [](const auto& port) { return port.id == "audio_in"; });
    const auto preview_output = std::find_if(preview_geometry.ports.begin(), preview_geometry.ports.end(),
        [](const auto& port) { return port.id == "audio_out"; });
    REQUIRE(preview_input != preview_geometry.ports.end());
    REQUIRE(preview_output != preview_geometry.ports.end());
    const audioalgo::editor::NodePositionPreview position_preview{
        preview_node.id, {static_cast<float>(preview_node.x), static_cast<float>(preview_node.y)}};
    const auto incoming_during_drag = audioalgo::editor::make_connection_geometry(
        project, geometry_registry, project.connections[0], preview_transform, position_preview);
    const auto outgoing_during_drag = audioalgo::editor::make_connection_geometry(
        project, geometry_registry, project.connections[1], preview_transform, position_preview);
    REQUIRE(incoming_during_drag.has_value());
    REQUIRE(outgoing_during_drag.has_value());
    REQUIRE(incoming_during_drag->finish.x == preview_input->anchor.x);
    REQUIRE(incoming_during_drag->finish.y == preview_input->anchor.y);
    REQUIRE(outgoing_during_drag->start.x == preview_output->anchor.x);
    REQUIRE(outgoing_during_drag->start.y == preview_output->anchor.y);
    REQUIRE(project.nodes[1].x == 220.0);
    REQUIRE(project.nodes[1].y == 30.0);
    REQUIRE(project.revision == original_revision);

    audioalgo::editor::ProjectDirtyState dirty_state;
    REQUIRE_FALSE(dirty_state.is_modified());
    REQUIRE(audioalgo::editor::move_node_layout(project, dirty_state, "gain", 310.0, 180.0));
    REQUIRE(dirty_state.is_modified());
    REQUIRE(project.revision == original_revision);
    REQUIRE(project.nodes[1].x == 310.0);
    REQUIRE(project.nodes[1].y == 180.0);
    REQUIRE_FALSE(audioalgo::editor::move_node_layout(project, dirty_state, "missing", 0.0, 0.0));

    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    RunService service(descriptors);
    const auto before = service.run({make_project(), project_path, temporary.path / "before-run", 127});
    const auto after = service.run({project, project_path, temporary.path / "after-run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(before));
    REQUIRE(std::holds_alternative<RunSuccess>(after));
    const auto& before_result = std::get<RunSuccess>(before);
    const auto& after_result = std::get<RunSuccess>(after);
    REQUIRE(before_result.output_sha256 == after_result.output_sha256);
    REQUIRE(before_result.input_sha256 == after_result.input_sha256);
    REQUIRE(before_result.frames == after_result.frames);

    const auto before_audio = read_wav_file(before_result.output_file);
    const auto after_audio = read_wav_file(after_result.output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(before_audio));
    REQUIRE(std::holds_alternative<AudioBuffer>(after_audio));
    const auto& first = std::get<AudioBuffer>(before_audio);
    const auto& second = std::get<AudioBuffer>(after_audio);
    REQUIRE(first.channel_count() == second.channel_count());
    REQUIRE(first.frame_count() == second.frame_count());
    for (std::size_t channel = 0; channel < first.channel_count(); ++channel)
        for (std::size_t frame = 0; frame < first.frame_count(); ++frame)
            REQUIRE(first.channel(channel)[frame] == second.channel(channel)[frame]);

    dirty_state.mark_saved();
    REQUIRE_FALSE(dirty_state.is_modified());
}

TEST_CASE("NED-09.b", "[node_interaction][NED-09]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "recreated-output.aaproj";
    const auto input_path = temporary.path / "input.wav";
    AudioBuffer input(48000, {"left", "right"}, 257);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.3 * std::sin(frame * 0.041));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.16 * std::cos(frame * 0.029));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(write_wav_file(input_path, input, WavEncoding::float32)));

    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    auto project = make_project();
    REQUIRE(audioalgo::editor::delete_node(project, descriptors, "output"));
    REQUIRE(project.selected_output_node_id.empty());
    REQUIRE(audioalgo::editor::try_add_node_from_descriptor(
        project, descriptors, "builtin.output", "output-recreated", 640.0, 60.0));
    REQUIRE(project.selected_output_node_id == "output-recreated");
    const auto revision_with_sink = project.revision;
    const auto node_count_with_sink = project.nodes.size();
    const auto connection_count_with_sink = project.connections.size();
    REQUIRE_FALSE(audioalgo::editor::try_add_node_from_descriptor(
        project, descriptors, "builtin.output", "output-second", 840.0, 60.0));
    REQUIRE(project.selected_output_node_id == "output-recreated");
    REQUIRE(project.revision == revision_with_sink);
    REQUIRE(project.nodes.size() == node_count_with_sink);
    REQUIRE(project.connections.size() == connection_count_with_sink);

    REQUIRE(audioalgo::try_add_connection(project, {"gain", "audio_out", "output-recreated", "audio"}, descriptors).empty());
    REQUIRE(project.revision == revision_with_sink + 1);
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    RunService service(descriptors);
    const auto outcome = service.run({project, project_path, temporary.path / "recreated-run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(outcome));
    const auto& result = std::get<RunSuccess>(outcome);
    REQUIRE(result.frames == input.frame_count());
    REQUIRE(std::filesystem::exists(result.complete_file));
    REQUIRE(result.output_sha256.size() == 64);
}

TEST_CASE("NED-09.d", "[node_interaction][NED-09]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "recreated-output-roundtrip.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 479);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.22 * std::sin(frame * 0.037));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.11 * std::cos(frame * 0.053));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(temporary.path / "input.wav", input, WavEncoding::float32)));

    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    auto project = make_project();
    REQUIRE(audioalgo::editor::delete_node(project, descriptors, "output"));
    REQUIRE(audioalgo::editor::try_add_node_from_descriptor(
        project, descriptors, "builtin.output", "output-reopened", 650.0, 30.0));
    REQUIRE(audioalgo::try_add_connection(
        project, {"gain", "audio_out", "output-reopened", "audio"}, descriptors).empty());
    REQUIRE(project.selected_output_node_id == "output-reopened");
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.selected_output_node_id == "output-reopened");
    REQUIRE(reopened.revision == project.revision);

    RunService service(descriptors);
    const auto outcome = service.run({reopened, project_path, temporary.path / "reopened-run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(outcome));
    const auto& result = std::get<RunSuccess>(outcome);
    REQUIRE(result.frames == input.frame_count());
    REQUIRE(std::filesystem::exists(result.complete_file));
}

TEST_CASE("NED-13.b", "[node_interaction][NED-13]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "inserted-input-preservation.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 257);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.31 * std::sin(frame * 0.043));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.17 * std::cos(frame * 0.029));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));
    const auto input_bytes_before_save = read_file_bytes(input_path);
    REQUIRE_FALSE(input_bytes_before_save.empty());

    auto project = make_project();
    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "builtin.gain", "saved-input-preservation-gain",
        {"gain", "audio_out", "output", "audio"}, 324.0, 54.0);
    REQUIRE(inserted.has_value());
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.project_id == project.project_id);
    REQUIRE(std::get<std::string>(reopened.nodes.front().parameters.at("path")) == "input.wav");
    REQUIRE(read_file_bytes(input_path) == input_bytes_before_save);

    RunService service(descriptors);
    const auto original_run = service.run({project, project_path, temporary.path / "original-run", 127});
    const auto reopened_run = service.run({reopened, project_path, temporary.path / "reopened-run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(original_run));
    REQUIRE(std::holds_alternative<RunSuccess>(reopened_run));
    const auto& original_result = std::get<RunSuccess>(original_run);
    const auto& reopened_result = std::get<RunSuccess>(reopened_run);
    REQUIRE(original_result.input_sha256.size() == 64);
    REQUIRE(reopened_result.input_sha256 == original_result.input_sha256);
    REQUIRE(read_file_bytes(input_path) == input_bytes_before_save);
}

TEST_CASE("NED-13.c", "[node_interaction][NED-13]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "inserted-oracle-roundtrip.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.38 * std::sin(frame * 0.031));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.21 * std::cos(frame * 0.047));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));

    auto project = make_project();
    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "builtin.gain", "oracle-inserted-gain",
        {"gain", "audio_out", "output", "audio"}, 326.0, 62.0);
    REQUIRE(inserted.has_value());
    const auto inserted_node = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "oracle-inserted-gain";
    });
    REQUIRE(inserted_node != project.nodes.end());
    std::get<double>(inserted_node->parameters.at("gain_db")) = -6.0;
    ++project.revision;
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.selected_output_node_id == "output");
    REQUIRE(std::any_of(reopened.connections.begin(), reopened.connections.end(), [](const auto& edge) {
        return edge.from_node == "gain" && edge.from_port == "audio_out" &&
               edge.to_node == "oracle-inserted-gain" && edge.to_port == "audio_in";
    }));
    REQUIRE(std::any_of(reopened.connections.begin(), reopened.connections.end(), [](const auto& edge) {
        return edge.from_node == "oracle-inserted-gain" && edge.from_port == "audio_out" &&
               edge.to_node == "output" && edge.to_port == "audio";
    }));

    RunService service(descriptors);
    const auto outcome = service.run({reopened, project_path, temporary.path / "inserted-oracle-run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(outcome));
    const auto& result = std::get<RunSuccess>(outcome);
    REQUIRE(result.frames == input.frame_count());
    const auto output = read_wav_file(result.output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(output));
    const auto& rendered = std::get<AudioBuffer>(output);
    REQUIRE(rendered.channel_count() == input.channel_count());
    REQUIRE(rendered.frame_count() == input.frame_count());

    const auto gain_product = std::pow(10.0, 6.0 / 20.0) * std::pow(10.0, -6.0 / 20.0);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const auto oracle = static_cast<double>(input.channel(channel)[frame]) * gain_product;
            REQUIRE(std::abs(static_cast<double>(rendered.channel(channel)[frame]) - oracle) <= 2.0e-6);
        }
    }
}

TEST_CASE("NED-14.a", "[node_interaction][NED-14]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "new-graph.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 257);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.27 * std::sin(frame * 0.039));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.13 * std::cos(frame * 0.057));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));

    ProjectModel project;
    project.project_id = "ned14-a-new-graph-project";
    project.name = "New graph workflow";
    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    auto source = audioalgo::editor::make_node_instance(
        descriptors, project.nodes, "builtin.file_source", "new-file", 40.0, 60.0);
    REQUIRE(source.has_value());
    source->parameters["path"] = std::string("input.wav");
    project.nodes.push_back(std::move(*source));
    ++project.revision;
    REQUIRE(audioalgo::editor::try_add_node_from_descriptor(
        project, descriptors, "builtin.gain", "new-gain", 270.0, 60.0));
    REQUIRE(audioalgo::editor::try_add_node_from_descriptor(
        project, descriptors, "builtin.output", "new-output", 500.0, 60.0));
    REQUIRE(project.selected_output_node_id == "new-output");
    REQUIRE(audioalgo::try_add_connection(
        project, {"new-file", "audio", "new-gain", "audio_in"}, descriptors).empty());
    REQUIRE(audioalgo::try_add_connection(
        project, {"new-gain", "audio_out", "new-output", "audio"}, descriptors).empty());
    REQUIRE(project.nodes.size() == 3);
    REQUIRE(project.connections.size() == 2);
    REQUIRE(audioalgo::validate_project(project, descriptors).empty());
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    RunService service(descriptors);
    const auto outcome = service.run({project, project_path, temporary.path / "new-graph-run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(outcome));
    const auto& result = std::get<RunSuccess>(outcome);
    const auto metadata = read_run_metadata(result.manifest_file);
    REQUIRE(metadata.has_value());
    REQUIRE(metadata->project_id == project.project_id);
    REQUIRE(metadata->project_revision == project.revision);
    REQUIRE(metadata->run_id == result.run_id);
    REQUIRE(metadata->input_sha256 == result.input_sha256);
    REQUIRE(metadata->output_sha256 == result.output_sha256);
    const auto rendered_result = read_wav_file(result.output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(rendered_result));
    const auto& rendered = std::get<AudioBuffer>(rendered_result);
    const auto gain = std::pow(10.0, 0.0 / 20.0);
    REQUIRE(rendered.channel_count() == input.channel_count());
    REQUIRE(rendered.frame_count() == input.frame_count());
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel)
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const auto oracle = static_cast<double>(input.channel(channel)[frame]) * gain;
            REQUIRE(std::abs(static_cast<double>(rendered.channel(channel)[frame]) - oracle) <= 2.0e-6);
        }
}

TEST_CASE("NED-14.b", "[node_interaction][NED-14]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "inserted-unity.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.41 * std::sin(frame * 0.033));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.19 * std::cos(frame * 0.051));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));

    auto project = make_project();
    project.project_id = "ned14-b-inserted-unity-project";
    project.name = "Inserted gain unity acceptance";
    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "builtin.gain", "inserted-minus-six-gain",
        {"gain", "audio_out", "output", "audio"}, 326.0, 62.0);
    REQUIRE(inserted.has_value());
    const auto inserted_node = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "inserted-minus-six-gain";
    });
    REQUIRE(inserted_node != project.nodes.end());
    std::get<double>(inserted_node->parameters.at("gain_db")) = -6.0;
    ++project.revision;
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.project_id == project.project_id);
    REQUIRE(reopened.revision == project.revision);
    REQUIRE(reopened.selected_output_node_id == "output");
    REQUIRE(reopened.nodes.size() == 4);
    REQUIRE(reopened.connections.size() == 3);

    RunService service(descriptors);
    const auto service_outcome = service.run(
        {reopened, project_path, temporary.path / "inserted-unity-service", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(service_outcome));
    const auto& service_result = std::get<RunSuccess>(service_outcome);
    const auto service_metadata = read_run_metadata(service_result.manifest_file);
    REQUIRE(service_metadata.has_value());

    const auto cli = run_cli(project_path, temporary.path / "inserted-unity-cli");
    REQUIRE(cli.started);
    REQUIRE(cli.finished);
    INFO(cli.output);
    REQUIRE(cli.exit_code == 0);
    const auto cli_metadata = read_run_metadata(temporary.path / "inserted-unity-cli" / "run.json");
    REQUIRE(cli_metadata.has_value());
    REQUIRE(service_metadata->project_id == project.project_id);
    REQUIRE(cli_metadata->project_id == project.project_id);
    REQUIRE(service_metadata->project_revision == project.revision);
    REQUIRE(cli_metadata->project_revision == project.revision);
    REQUIRE(service_metadata->run_id == service_result.run_id);
    REQUIRE(cli_metadata->run_id != service_metadata->run_id);
    REQUIRE(cli_metadata->input_sha256 == service_metadata->input_sha256);
    REQUIRE(cli_metadata->output_sha256 == service_metadata->output_sha256);

    const auto service_audio_result = read_wav_file(service_result.output_file);
    const auto cli_audio_result = read_wav_file(temporary.path / "inserted-unity-cli" / "output.wav");
    REQUIRE(std::holds_alternative<AudioBuffer>(service_audio_result));
    REQUIRE(std::holds_alternative<AudioBuffer>(cli_audio_result));
    const auto& service_audio = std::get<AudioBuffer>(service_audio_result);
    const auto& cli_audio = std::get<AudioBuffer>(cli_audio_result);
    REQUIRE(service_audio.channel_count() == input.channel_count());
    REQUIRE(cli_audio.channel_count() == input.channel_count());
    REQUIRE(service_audio.frame_count() == input.frame_count());
    REQUIRE(cli_audio.frame_count() == input.frame_count());
    const auto gain_product = std::pow(10.0, 6.0 / 20.0) * std::pow(10.0, -6.0 / 20.0);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const auto oracle = static_cast<double>(input.channel(channel)[frame]) * gain_product;
            REQUIRE(std::abs(static_cast<double>(service_audio.channel(channel)[frame]) - oracle) <= 2.0e-6);
            REQUIRE(std::abs(static_cast<double>(cli_audio.channel(channel)[frame]) - oracle) <= 2.0e-6);
            REQUIRE(service_audio.channel(channel)[frame] == cli_audio.channel(channel)[frame]);
        }
    }
}

TEST_CASE("NED-14.c", "[node_interaction][NED-14]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "inserted-zero-gain.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.36 * std::sin(frame * 0.037));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.23 * std::cos(frame * 0.049));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));

    auto project = make_project();
    project.project_id = "ned14-c-inserted-zero-gain-project";
    project.name = "Inserted zero gain participation acceptance";
    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "builtin.gain", "inserted-zero-gain",
        {"gain", "audio_out", "output", "audio"}, 326.0, 62.0);
    REQUIRE(inserted.has_value());
    const auto inserted_node = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "inserted-zero-gain";
    });
    REQUIRE(inserted_node != project.nodes.end());
    REQUIRE(std::get<double>(inserted_node->parameters.at("gain_db")) == 0.0);
    ++project.revision;
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.project_id == project.project_id);
    REQUIRE(reopened.revision == project.revision);
    REQUIRE(reopened.nodes.size() == 4);
    REQUIRE(reopened.connections.size() == 3);

    RunService service(descriptors);
    const auto service_outcome = service.run(
        {reopened, project_path, temporary.path / "inserted-zero-service", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(service_outcome));
    const auto& service_result = std::get<RunSuccess>(service_outcome);
    const auto service_metadata = read_run_metadata(service_result.manifest_file);
    REQUIRE(service_metadata.has_value());

    const auto cli = run_cli(project_path, temporary.path / "inserted-zero-cli");
    REQUIRE(cli.started);
    REQUIRE(cli.finished);
    INFO(cli.output);
    REQUIRE(cli.exit_code == 0);
    const auto cli_metadata = read_run_metadata(temporary.path / "inserted-zero-cli" / "run.json");
    REQUIRE(cli_metadata.has_value());
    REQUIRE(service_metadata->project_id == project.project_id);
    REQUIRE(cli_metadata->project_id == project.project_id);
    REQUIRE(service_metadata->project_revision == project.revision);
    REQUIRE(cli_metadata->project_revision == project.revision);
    REQUIRE(service_metadata->run_id == service_result.run_id);
    REQUIRE(cli_metadata->run_id != service_metadata->run_id);
    REQUIRE(cli_metadata->input_sha256 == service_metadata->input_sha256);
    REQUIRE(cli_metadata->output_sha256 == service_metadata->output_sha256);

    const auto service_audio_result = read_wav_file(service_result.output_file);
    const auto cli_audio_result = read_wav_file(temporary.path / "inserted-zero-cli" / "output.wav");
    REQUIRE(std::holds_alternative<AudioBuffer>(service_audio_result));
    REQUIRE(std::holds_alternative<AudioBuffer>(cli_audio_result));
    const auto& service_audio = std::get<AudioBuffer>(service_audio_result);
    const auto& cli_audio = std::get<AudioBuffer>(cli_audio_result);
    REQUIRE(service_audio.channel_count() == input.channel_count());
    REQUIRE(cli_audio.channel_count() == input.channel_count());
    REQUIRE(service_audio.frame_count() == input.frame_count());
    REQUIRE(cli_audio.frame_count() == input.frame_count());
    const auto gain = std::pow(10.0, 6.0 / 20.0);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const auto oracle = static_cast<double>(input.channel(channel)[frame]) * gain;
            REQUIRE(std::abs(static_cast<double>(service_audio.channel(channel)[frame]) - oracle) <= 2.0e-6);
            REQUIRE(std::abs(static_cast<double>(cli_audio.channel(channel)[frame]) - oracle) <= 2.0e-6);
            REQUIRE(service_audio.channel(channel)[frame] == cli_audio.channel(channel)[frame]);
        }
    }
}

TEST_CASE("NED-14.d", "[node_interaction][NED-14]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "inserted-reconnect.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 385);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.34 * std::sin(frame * 0.035));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.2 * std::cos(frame * 0.061));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));

    auto project = make_project();
    project.project_id = "ned14-d-reconnect-project";
    project.name = "Disconnect reconnect persistence";
    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    const audioalgo::Connection original_output_edge{
        "gain", "audio_out", "output", "audio"};
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "builtin.gain", "reconnect-inserted-gain",
        original_output_edge, 326.0, 62.0);
    REQUIRE(inserted.has_value());
    const auto inserted_node = std::find_if(project.nodes.begin(), project.nodes.end(), [](const auto& node) {
        return node.id == "reconnect-inserted-gain";
    });
    REQUIRE(inserted_node != project.nodes.end());
    std::get<double>(inserted_node->parameters.at("gain_db")) = -6.0;
    ++project.revision;
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    RunService service(descriptors);
    const audioalgo::Connection inserted_output_edge{
        "reconnect-inserted-gain", "audio_out", "output", "audio"};
    REQUIRE(audioalgo::editor::try_disconnect_connection(project, inserted_output_edge));
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    const auto disconnected_output = temporary.path / "disconnected-service";
    const auto disconnected_service = service.run({
        project, project_path, disconnected_output, 127});
    const auto* disconnected_diagnostic = std::get_if<audioalgo::Diagnostic>(&disconnected_service);
    REQUIRE(disconnected_diagnostic != nullptr);
    REQUIRE(disconnected_diagnostic->code == "input_required");
    REQUIRE_FALSE(std::filesystem::exists(disconnected_output));
    const auto disconnected_cli_dir = temporary.path / "disconnected-cli";
    const auto disconnected_cli = run_cli(project_path, disconnected_cli_dir);
    REQUIRE(disconnected_cli.started);
    REQUIRE(disconnected_cli.finished);
    INFO(disconnected_cli.output);
    REQUIRE(disconnected_cli.exit_code == 1);
    REQUIRE(disconnected_cli.output.find("input_required") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(disconnected_cli_dir / "complete.json"));

    REQUIRE(audioalgo::try_add_connection(project, inserted_output_edge, descriptors).empty());
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    const auto reopened_result = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(reopened_result));
    const auto& reopened = std::get<ProjectModel>(reopened_result);
    REQUIRE(reopened.project_id == project.project_id);
    REQUIRE(reopened.revision == project.revision);
    REQUIRE(reopened.selected_output_node_id == "output");
    REQUIRE(reopened.nodes.size() == 4);
    REQUIRE(reopened.connections.size() == 3);
    REQUIRE(std::count_if(reopened.connections.begin(), reopened.connections.end(), [&](const auto& edge) {
        return edge.from_node == inserted_output_edge.from_node && edge.from_port == inserted_output_edge.from_port &&
               edge.to_node == inserted_output_edge.to_node && edge.to_port == inserted_output_edge.to_port;
    }) == 1);

    const auto service_success = service.run({
        reopened, project_path, temporary.path / "reconnected-service", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(service_success));
    const auto& service_result = std::get<RunSuccess>(service_success);
    const auto cli_success = run_cli(project_path, temporary.path / "reconnected-cli");
    REQUIRE(cli_success.started);
    REQUIRE(cli_success.finished);
    INFO(cli_success.output);
    REQUIRE(cli_success.exit_code == 0);
    const auto service_metadata = read_run_metadata(service_result.manifest_file);
    const auto cli_metadata = read_run_metadata(temporary.path / "reconnected-cli" / "run.json");
    REQUIRE(service_metadata.has_value());
    REQUIRE(cli_metadata.has_value());
    REQUIRE(service_metadata->project_id == reopened.project_id);
    REQUIRE(cli_metadata->project_id == reopened.project_id);
    REQUIRE(service_metadata->project_revision == reopened.revision);
    REQUIRE(cli_metadata->project_revision == reopened.revision);
    REQUIRE(cli_metadata->input_sha256 == service_metadata->input_sha256);
    REQUIRE(cli_metadata->output_sha256 == service_metadata->output_sha256);
    REQUIRE(std::filesystem::exists(service_result.complete_file));
    REQUIRE(std::filesystem::exists(temporary.path / "reconnected-cli" / "complete.json"));

    const auto service_audio_result = read_wav_file(service_result.output_file);
    const auto cli_audio_result = read_wav_file(temporary.path / "reconnected-cli" / "output.wav");
    REQUIRE(std::holds_alternative<AudioBuffer>(service_audio_result));
    REQUIRE(std::holds_alternative<AudioBuffer>(cli_audio_result));
    const auto& service_audio = std::get<AudioBuffer>(service_audio_result);
    const auto& cli_audio = std::get<AudioBuffer>(cli_audio_result);
    REQUIRE(service_audio.frame_count() == input.frame_count());
    REQUIRE(cli_audio.frame_count() == input.frame_count());
    const auto gain_product = std::pow(10.0, 6.0 / 20.0) * std::pow(10.0, -6.0 / 20.0);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel)
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const auto oracle = static_cast<double>(input.channel(channel)[frame]) * gain_product;
            REQUIRE(std::abs(static_cast<double>(service_audio.channel(channel)[frame]) - oracle) <= 2.0e-6);
            REQUIRE(std::abs(static_cast<double>(cli_audio.channel(channel)[frame]) - oracle) <= 2.0e-6);
            REQUIRE(service_audio.channel(channel)[frame] == cli_audio.channel(channel)[frame]);
        }
}

TEST_CASE("NED-14.e", "[node_interaction][NED-14]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "correlated-inserted-project.aaproj";
    AudioBuffer input(48000, {"left", "right"}, 129);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.29 * std::sin(frame * 0.071));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.14 * std::cos(frame * 0.053));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));

    auto project = make_project();
    project.project_id = "ned14-e-correlated-project";
    project.name = "Correlated inserted graph";
    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    const auto inserted = audioalgo::editor::try_insert_library_node_on_connection(
        project, descriptors, "builtin.gain", "correlated-inserted-gain",
        {"gain", "audio_out", "output", "audio"}, 326.0, 62.0);
    REQUIRE(inserted.has_value());
    REQUIRE(project.nodes.size() == 4);
    REQUIRE(project.connections.size() == 3);
    REQUIRE(std::none_of(project.connections.begin(), project.connections.end(), [](const auto& edge) {
        return edge.from_node == "gain" && edge.from_port == "audio_out" &&
               edge.to_node == "output" && edge.to_port == "audio";
    }));
    REQUIRE(std::any_of(project.connections.begin(), project.connections.end(), [](const auto& edge) {
        return edge.from_node == "file" && edge.from_port == "audio" &&
               edge.to_node == "gain" && edge.to_port == "audio_in";
    }));
    REQUIRE(std::any_of(project.connections.begin(), project.connections.end(), [](const auto& edge) {
        return edge.from_node == "gain" && edge.from_port == "audio_out" &&
               edge.to_node == "correlated-inserted-gain" && edge.to_port == "audio_in";
    }));
    REQUIRE(std::any_of(project.connections.begin(), project.connections.end(), [](const auto& edge) {
        return edge.from_node == "correlated-inserted-gain" && edge.from_port == "audio_out" &&
               edge.to_node == "output" && edge.to_port == "audio";
    }));
    for (const auto& node : project.nodes)
        REQUIRE(std::any_of(project.connections.begin(), project.connections.end(), [&](const auto& edge) {
            return edge.from_node == node.id || edge.to_node == node.id;
        }));
    REQUIRE(audioalgo::validate_project(project, descriptors).empty());
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));

    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.project_id == project.project_id);
    REQUIRE(reopened.revision == project.revision);
    REQUIRE(reopened.nodes.size() == 4);
    REQUIRE(reopened.connections.size() == 3);

    RunService service(descriptors);
    const auto service_result = service.run({
        reopened, project_path, temporary.path / "correlated-service", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(service_result));
    const auto& service_success = std::get<RunSuccess>(service_result);
    const auto cli = run_cli(project_path, temporary.path / "correlated-cli");
    REQUIRE(cli.started);
    REQUIRE(cli.finished);
    INFO(cli.output);
    REQUIRE(cli.exit_code == 0);
    const auto service_metadata = read_run_metadata(service_success.manifest_file);
    const auto cli_metadata = read_run_metadata(temporary.path / "correlated-cli" / "run.json");
    REQUIRE(service_metadata.has_value());
    REQUIRE(cli_metadata.has_value());
    REQUIRE(service_metadata->project_id == reopened.project_id);
    REQUIRE(cli_metadata->project_id == reopened.project_id);
    REQUIRE(service_metadata->project_revision == reopened.revision);
    REQUIRE(cli_metadata->project_revision == reopened.revision);
    REQUIRE(service_metadata->run_id == service_success.run_id);
    REQUIRE(cli_metadata->run_id != service_metadata->run_id);
    REQUIRE(service_metadata->input_sha256 == cli_metadata->input_sha256);
    REQUIRE(service_metadata->output_sha256 == service_success.output_sha256);
    REQUIRE(cli_metadata->output_sha256 == service_success.output_sha256);
    REQUIRE(std::filesystem::exists(service_success.complete_file));
    REQUIRE(std::filesystem::exists(temporary.path / "correlated-cli" / "complete.json"));
    REQUIRE(std::filesystem::exists(service_success.output_file));
    REQUIRE(std::filesystem::exists(temporary.path / "correlated-cli" / "output.wav"));
}

TEST_CASE("NED-01.b", "[node_interaction][NED-01]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_filename = std::filesystem::u8path("输入 tone final.wav");
    const auto input_path = temporary.path / input_filename;
    const auto project_path = temporary.path / std::filesystem::u8path("中文 工程 file.aaproj");
    AudioBuffer input(48000, {"left", "right"}, 321);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.28 * std::sin(frame * 0.047));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.15 * std::cos(frame * 0.039));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        write_wav_file(input_path, input, WavEncoding::float32)));
    const auto original_input_bytes = read_file_bytes(input_path);
    REQUIRE_FALSE(original_input_bytes.empty());

    auto project = make_project();
    project.project_id = "ned01-b-chinese-path-project";
    project.name = "中文工程 · −6 dB …";
    std::get<std::string>(project.nodes.front().parameters.at("path")) = path_to_utf8(input_filename);
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);
    REQUIRE(reopened.name == "中文工程 · −6 dB …");
    REQUIRE(std::get<std::string>(reopened.nodes.front().parameters.at("path")) == path_to_utf8(input_filename));
    REQUIRE(read_file_bytes(input_path) == original_input_bytes);

    NodeRegistry descriptors;
    register_builtin_nodes(descriptors);
    RunService service(descriptors);
    const auto service_outcome = service.run({
        reopened, project_path, temporary.path / "中文 service run", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(service_outcome));
    const auto& service_result = std::get<RunSuccess>(service_outcome);
    const auto cli = run_cli(project_path, temporary.path / "中文 cli run");
    REQUIRE(cli.started);
    REQUIRE(cli.finished);
    INFO(cli.output);
    REQUIRE(cli.exit_code == 0);
    const auto service_metadata = read_run_metadata(service_result.manifest_file);
    const auto cli_metadata = read_run_metadata(temporary.path / "中文 cli run" / "run.json");
    REQUIRE(service_metadata.has_value());
    REQUIRE(cli_metadata.has_value());
    REQUIRE(service_metadata->input_file == path_to_utf8(input_filename));
    REQUIRE(cli_metadata->input_file == path_to_utf8(input_filename));
    REQUIRE(service_metadata->input_sha256 == cli_metadata->input_sha256);
    REQUIRE(service_metadata->input_sha256 == service_result.input_sha256);
    REQUIRE(read_file_bytes(input_path) == original_input_bytes);
}

TEST_CASE("AR-05.e a saved Gain project replays through one registered implementation", "[R1][AR-05.e]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "legacy-gain.aaproj";
    const auto input_path = temporary.path / "input.wav";
    AudioBuffer input(48000, {"left", "right"}, 641);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.42 * std::sin(frame * 0.027));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.23 * std::cos(frame * 0.039));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(write_wav_file(input_path, input, WavEncoding::float32)));

    // Exactly one built-in catalog, and the composition root refuses to register twice.
    // The catalog grew to 5 descriptors when A2 added the production builtin.mixer.
    NodeRegistry descriptors;
    REQUIRE(register_builtin_nodes(descriptors));
    CHECK(descriptors.descriptors().size() == 5);
    CHECK_FALSE(register_builtin_nodes(descriptors));
    CHECK(descriptors.descriptors().size() == 5);
    CHECK(descriptors.find("builtin.gain") != nullptr);
    CHECK(descriptors.find("builtin.gain")->version == "1.0.0");
    CHECK(descriptors.create_processor("builtin.gain", "gain-probe").processor != nullptr);

    const auto project = make_project();
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_path, project)));
    const auto loaded = load_project_file(project_path);
    REQUIRE(std::holds_alternative<ProjectModel>(loaded));
    const auto& reopened = std::get<ProjectModel>(loaded);

    RunService service(descriptors);
    const auto first = service.run({project, project_path, temporary.path / "legacy-first", 127});
    const auto second = service.run({reopened, project_path, temporary.path / "legacy-second", 127});
    REQUIRE(std::holds_alternative<RunSuccess>(first));
    REQUIRE(std::holds_alternative<RunSuccess>(second));
    const auto& first_result = std::get<RunSuccess>(first);
    const auto& second_result = std::get<RunSuccess>(second);
    CHECK(first_result.output_sha256 == second_result.output_sha256);
    CHECK(first_result.frames == second_result.frames);
    CHECK(first_result.frames == input.frame_count());
    CHECK(first_result.input_sha256 == second_result.input_sha256);

    // The playback data path reads the published artifact and matches the independent oracle.
    const auto published = read_wav_file(first_result.output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(published));
    const auto& playback = std::get<AudioBuffer>(published);
    REQUIRE(playback.channel_count() == input.channel_count());
    REQUIRE(playback.frame_count() == input.frame_count());
    const double gain = std::pow(10.0, 6.0 / 20.0);
    for (std::size_t channel = 0; channel < input.channel_count(); ++channel) {
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            const double expected = static_cast<double>(input.channel(channel)[frame]) * gain;
            const double tolerance = 2.0e-6 + 1.0e-5 * std::abs(expected);
            REQUIRE(std::abs(static_cast<double>(playback.channel(channel)[frame]) - expected) <= tolerance);
        }
    }
}

namespace {
class SnapshotMutationProbe final : public audioalgo::AudioNodeProcessor {
public:
    explicit SnapshotMutationProbe(std::function<void()> mutate) : mutate_(std::move(mutate)) {}
    bool supports_block_processing() const noexcept override { return true; }
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest& request) override { return probe_.process(request); }
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext& context) override {
        mutate_();
        return probe_.prepare(context);
    }
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& block) override { return probe_.process_block(block); }
private:
    std::function<void()> mutate_;
    audioalgo::tests::ParameterProbeNode probe_;
};
}

TEST_CASE("AR-16.a native resource versions remain fixed when originals change before prepare", "[AR-16.a][R5][native_integration]") {
    juce::ScopedJuceInitialiser_GUI messages;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto coefficient_path = temporary.path / "coefficient.txt";
    AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(static_cast<int>(frame % 17) - 8) / 64.0f;
        input.mutable_channel(1)[frame] = static_cast<float>(static_cast<int>(frame % 31) - 15) / 128.0f;
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(write_wav_file(input_path,input,WavEncoding::float32)));
    { std::ofstream stream(coefficient_path); stream << "0.25\n"; }
    const auto original_input = read_file_bytes(input_path);
    const auto original_coefficient = read_file_bytes(coefficient_path);
    auto project = make_project();
    project.project_id="native-resource-snapshot";
    project.name="Native immutable resource experiment";
    project.nodes[1].type_id = "test.parameter_probe";
    project.nodes[1].parameters = {{"enabled", true}, {"coefficient", std::string("coefficient.txt")}};
    project.connections = {{"file","audio","gain","in"},{"gain","out","output","audio"}};
    const auto project_file = temporary.path / "snapshot.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(project_file,project)));
    bool did_mutate = false;
    const bool remove_originals = GENERATE(false,true);
    NodeRegistry nodes;
    REQUIRE(register_builtin_nodes(nodes));
    REQUIRE(nodes.register_node(audioalgo::tests::parameter_probe_descriptor(),[&] {
        return std::make_unique<SnapshotMutationProbe>([&] {
            did_mutate=true;
            project.revision=99;
            project.nodes[1].parameters["enabled"]=false;
            if (remove_originals) {
                std::filesystem::remove(input_path);
                std::filesystem::remove(coefficient_path);
                std::filesystem::remove(project_file);
            } else {
                AudioBuffer changed(48000,{"left","right"},513);
                const auto replacement = temporary.path /
                    ("replacement-" + juce::Uuid().toString().toStdString() + ".wav");
                if (!std::holds_alternative<audioalgo::WavWriteSummary>(
                        write_wav_file(replacement,changed,WavEncoding::float32)) ||
                    !std::filesystem::copy_file(replacement,input_path,std::filesystem::copy_options::overwrite_existing))
                    throw std::runtime_error("Fixture replacement WAV failed");
                std::ofstream stream(coefficient_path); stream << "9\n";
            }
        });
    }).registered);
    RunService service(nodes);
    const auto result = service.run({project,project_file,temporary.path/"frozen-run",127});
    CHECK(did_mutate);
    if (remove_originals) {
        CHECK_FALSE(std::filesystem::exists(input_path));
        CHECK_FALSE(std::filesystem::exists(coefficient_path));
        CHECK_FALSE(std::filesystem::exists(project_file));
    } else {
        CHECK((read_file_bytes(input_path)!=original_input));
        CHECK((read_file_bytes(coefficient_path)!=original_coefficient));
    }
    INFO((std::holds_alternative<RunSuccess>(result) ? "success" : std::get<audioalgo::Diagnostic>(result).code + ": " + std::get<audioalgo::Diagnostic>(result).message));
    const auto* success=std::get_if<RunSuccess>(&result);
    REQUIRE(success!=nullptr);
    const auto read=read_wav_file(success->output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(read));
    const auto& output=std::get<AudioBuffer>(read);
    CHECK(output.frame_count()==513);
    for (std::size_t channel=0;channel<2;++channel)
        for (std::size_t frame=0;frame<513;++frame)
            CHECK(output.channel(channel)[frame]==Catch::Approx(input.channel(channel)[frame]*-0.375).margin(2e-6));
    CHECK(project.revision==99);
    const auto manifest=juce::JSON::parse(juce::File(juce::String::fromUTF8(path_to_utf8(success->manifest_file).c_str())).loadFileAsString());
    CHECK(static_cast<int>(manifest["project_revision"])==4);
    REQUIRE(manifest["resources"].isArray());
    CHECK(manifest["resources"].getArray()->size()==2);
    for (const auto& resource:*manifest["resources"].getArray()) {
        const auto copy=temporary.path/"frozen-run"/std::filesystem::u8path(resource["snapshot_path"].toString().toStdString());
        const auto bytes=read_file_bytes(copy);
        CHECK(bytes==(resource["node_id"].toString()=="file"?original_input:original_coefficient));
        CHECK(resource["snapshot_sha256"].toString()==resource["sha256"].toString());
    }
    const auto replay_file=temporary.path/"frozen-run/replay.aaproj";
    const auto replay_project=load_project_file(replay_file);
    REQUIRE(std::holds_alternative<ProjectModel>(replay_project));
    const auto repeated=service.run({std::get<ProjectModel>(replay_project),replay_file,temporary.path/"replayed-run",127});
    REQUIRE(std::holds_alternative<RunSuccess>(repeated));
    const auto repeated_audio=read_wav_file(std::get<RunSuccess>(repeated).output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(repeated_audio));
    for (std::size_t channel=0;channel<2;++channel)
        for (std::size_t frame=0;frame<513;++frame)
            CHECK(std::get<AudioBuffer>(repeated_audio).channel(channel)[frame]==Catch::Approx(input.channel(channel)[frame]*-0.375).margin(2e-6));
    CHECK(std::get<RunSuccess>(repeated).run_id!=success->run_id);
    if (const auto* location=std::getenv("AUDIOALGO_SNAPSHOT_ARTIFACT_OUTPUT"); location && *location) {
        const auto target=std::filesystem::u8path(location)/(remove_originals?"remove":"replace");
        REQUIRE_FALSE(std::filesystem::exists(target));
        std::filesystem::create_directories(target);
        std::filesystem::copy(temporary.path/"frozen-run",target/"run",std::filesystem::copy_options::recursive);
        std::filesystem::copy(temporary.path/"replayed-run",target/"replay",std::filesystem::copy_options::recursive);
        { std::ofstream stream(target/"original.wav",std::ios::binary); stream.write(original_input.data(),static_cast<std::streamsize>(original_input.size())); }
        { std::ofstream stream(target/"original-coefficient.txt",std::ios::binary); stream.write(original_coefficient.data(),static_cast<std::streamsize>(original_coefficient.size())); }
    }
}

TEST_CASE("AR-10.c runtime parameter failures locate the instance and recover through rendering", "[AR-10.c][R3][native_integration]") {
    juce::ScopedJuceInitialiser_GUI messages;
    TemporaryDirectory temporary;
    AudioBuffer input(48000,{"left","right"},129);
    for (std::size_t frame=0;frame<129;++frame) {
        input.mutable_channel(0)[frame]=0.25f;
        input.mutable_channel(1)[frame]=-0.125f;
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(write_wav_file(temporary.path/"input.wav",input,WavEncoding::float32)));
    const auto coefficient=temporary.path/"coefficient.txt";
    { std::ofstream stream(coefficient);stream << "invalid-coefficient\n"; }
    const auto invalid_bytes=read_file_bytes(coefficient);
    NodeRegistry nodes;
    REQUIRE(register_builtin_nodes(nodes));
    REQUIRE(nodes.register_node(audioalgo::tests::parameter_probe_descriptor(),[] {
        return std::make_unique<audioalgo::tests::ParameterProbeNode>();
    }).registered);
    auto project=make_project();
    project.nodes[1].type_id="test.parameter_probe";
    project.nodes[1].parameters={{"enabled",true},{"coefficient",std::string("coefficient.txt")}};
    project.connections={{"file","audio","gain","in"},{"gain","out","output","audio"}};
    const auto file=temporary.path/"parameter-recovery.aaproj";
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(file,project)));
    audioalgo::editor::EditorController controller(nodes);
    controller.adopt_project(project);
    RunService service(nodes);
    const auto failed=service.run({controller.project(),file,temporary.path/"failed",127});
    const auto* diagnostic=std::get_if<audioalgo::Diagnostic>(&failed);
    REQUIRE(diagnostic!=nullptr);
    CHECK(diagnostic->code=="coefficient_invalid");
    CHECK(diagnostic->node_id=="gain");
    CHECK(diagnostic->port_id=="coefficient");
    CHECK(diagnostic->message.find("gain")!=std::string::npos);
    CHECK((read_file_bytes(coefficient)==invalid_bytes));
    CHECK_FALSE(std::filesystem::exists(temporary.path/"failed/complete.json"));
    { std::ofstream stream(coefficient);stream << "0.25\n"; }
    const auto recovered=service.run({controller.project(),file,temporary.path/"recovered",127});
    REQUIRE(std::holds_alternative<RunSuccess>(recovered));
    const auto output=read_wav_file(std::get<RunSuccess>(recovered).output_file);
    REQUIRE(std::holds_alternative<AudioBuffer>(output));
    CHECK(std::get<AudioBuffer>(output).frame_count()==129);
    for (std::size_t channel=0;channel<2;++channel)
        for (std::size_t frame=0;frame<129;++frame)
            CHECK(std::get<AudioBuffer>(output).channel(channel)[frame]==Catch::Approx(input.channel(channel)[frame]*-0.375).margin(2e-6));
    CHECK(controller.project().revision==project.revision);
}
