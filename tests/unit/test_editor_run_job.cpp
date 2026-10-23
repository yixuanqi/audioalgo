#include "../../apps/editor/EditorRunJob.h"
#include "../../apps/editor/EditorRunCompletion.h"
#include "../../apps/editor/EditorWorkerPool.h"
#include "audio_editor_application/EditorController.h"
#include "audio_io/WavFile.h"
#include "audio_project/ProjectFileIO.h"
#include "audio_run_service/RunService.h"
#include "plugins/builtin/registration.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::current_path() / ".audioalgo-test-tmp" /
        std::filesystem::u8path("production RunJob " + juce::Uuid().toString().toStdString());
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

audioalgo::ProjectModel make_gain_project() {
    audioalgo::ProjectModel project;
    project.project_id = "production-runjob-project";
    project.name = "Production RunJob";
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

struct DeliveryTarget final : juce::Component {
    int deliveries = 0;
    std::thread::id delivery_thread;
    std::optional<audioalgo::RunOutcome> outcome;
    audioalgo::editor::WaveformChannels waveform;
    audioalgo::RunIdentity identity;

    void receive(audioalgo::RunOutcome result, audioalgo::editor::WaveformChannels view,
                 audioalgo::RunIdentity request_identity) {
        ++deliveries;
        delivery_thread = std::this_thread::get_id();
        outcome = std::move(result);
        waveform = std::move(view);
        identity = std::move(request_identity);
    }
};

struct RunGate : juce::Thread::Listener {
    void exitSignalSent() override { exit_signal.store(true); }
    void block_once() {
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return released; });
    }

    void release() {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        condition.notify_all();
    }

    [[nodiscard]] bool has_entered() {
        std::lock_guard lock(mutex);
        return entered;
    }

    [[nodiscard]] bool has_released() {
        std::lock_guard lock(mutex);
        return released;
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    std::atomic<bool> exit_signal{false};
    std::atomic<std::size_t> completed_blocks{0};
};

class RunGateProcessor final : public audioalgo::AudioNodeProcessor {
public:
    explicit RunGateProcessor(std::shared_ptr<RunGate> gate) : gate_(std::move(gate)) {}

    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override {
        return audioalgo::Diagnostic{"test_block_path_required", "The test gate requires block processing"};
    }

    [[nodiscard]] bool supports_block_processing() const noexcept override { return true; }

    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& block) override {
        if (!waited_.exchange(true))
            gate_->block_once();
        for (std::size_t channel = 0; channel < block.channel_count; ++channel)
            std::copy_n(block.input_channels[channel], block.frame_count, block.output_channels[channel]);
        gate_->completed_blocks.fetch_add(1);
        return audioalgo::AudioBlockResult{block.frame_count};
    }

private:
    std::shared_ptr<RunGate> gate_;
    std::atomic<bool> waited_{false};
};

audioalgo::ProjectModel make_blocked_gain_project() {
    auto project = make_gain_project();
    project.project_id = "production-runjob-closed-project";
    project.nodes.insert(project.nodes.end() - 1,
                         {"gate", "test.run_gate", {}, 330.0, 30.0});
    project.connections[1] = {"gain", "audio_out", "gate", "audio_in"};
    project.connections.push_back({"gate", "audio_out", "output", "audio"});
    return project;
}

struct RunPresentationState {
    std::string result_id = "previous-result";
    std::filesystem::path output_file = "previous-output.wav";
    std::string status = "Previous run is ready";
    audioalgo::editor::WaveformChannels waveform{{{-0.5f, 0.5f}}};
    bool playback_enabled = true;
};

struct DeletedNodeCompletionTarget final : juce::Component {
    DeletedNodeCompletionTarget(audioalgo::editor::EditorController& controller,
                                std::uint64_t session_epoch, std::string selected_node_id)
        : controller(controller), session_epoch(session_epoch), selected_node_id(std::move(selected_node_id)) {}

    void receive(audioalgo::RunOutcome outcome, audioalgo::editor::WaveformChannels waveform,
                 audioalgo::RunIdentity identity) {
        ++service_notifications;
        observed_outcome = std::move(outcome);
        observed_identity = std::move(identity);

        accepted = audioalgo::editor::apply_if_current_run(
            reception, observed_identity, controller.project().project_id, session_epoch, controller.revision(), [&] {
                ++completion_handler_invocations;
                const auto selected = controller.node_snapshot(selected_node_id);
                if (!selected)
                    ++deleted_node_queries;

                if (const auto* error = std::get_if<audioalgo::Diagnostic>(&*observed_outcome)) {
                    presentation.status = error->code + ": " + error->message;
                    presentation.playback_enabled = false;
                    return;
                }
                const auto& success = std::get<audioalgo::RunSuccess>(*observed_outcome);
                presentation.result_id = success.run_id;
                presentation.output_file = success.output_file;
                presentation.status = "Completed " + success.run_id;
                presentation.waveform = std::move(waveform);
                presentation.playback_enabled = true;
            });
    }

    audioalgo::editor::EditorController& controller;
    std::uint64_t session_epoch;
    std::string selected_node_id;
    audioalgo::RunReception reception;
    RunPresentationState presentation;
    std::optional<audioalgo::RunOutcome> observed_outcome;
    audioalgo::RunIdentity observed_identity;
    int service_notifications = 0;
    int completion_handler_invocations = 0;
    int deleted_node_queries = 0;
    bool accepted = true;
};

} // namespace

TEST_CASE("AR-09.c delayed service completion cannot read a deleted node or update the current view",
          "[architecture][native_integration][lifecycle]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "delayed completion.aaproj";
    const auto input_path = temporary.path / "input.wav";
    audioalgo::AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.2 * std::sin(frame * 0.013));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.15 * std::cos(frame * 0.019));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        audioalgo::write_wav_file(input_path, input, audioalgo::WavEncoding::float32)));

    auto registry = std::make_shared<audioalgo::NodeRegistry>();
    REQUIRE(audioalgo::register_builtin_nodes(*registry));
    auto run_gate = std::make_shared<RunGate>();
    const audioalgo::NodeDescriptor gate_descriptor{
        "test.run_gate", "1.0.0", "Test run gate",
        {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
        {}, audioalgo::NodeExecutionKind::block};
    REQUIRE(registry->register_node(gate_descriptor, [run_gate] {
        return std::make_unique<RunGateProcessor>(run_gate);
    }).registered);

    audioalgo::editor::EditorController controller(*registry);
    const auto project_snapshot = make_blocked_gain_project();
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_path, project_snapshot)));
    controller.adopt_project(project_snapshot);
    constexpr std::uint64_t session_epoch = 73;
    const audioalgo::RunIdentity identity{controller.project().project_id, session_epoch,
                                          "delayed-after-delete", controller.revision()};
    audioalgo::RunRequest request{controller.project(), project_path, temporary.path / "runs" / "delayed", 127};
    request.identity = identity;

    auto target = std::make_unique<DeletedNodeCompletionTarget>(controller, session_epoch, "gain");
    target->reception.begin(identity);
    auto state = std::make_shared<audioalgo::editor::ComponentDispatchState<DeletedNodeCompletionTarget>>(*target);
    std::weak_ptr<audioalgo::editor::ComponentDispatchState<DeletedNodeCompletionTarget>> weak_state = state;
    const auto deliver = [](DeletedNodeCompletionTarget& live_target, audioalgo::RunOutcome outcome,
                            audioalgo::editor::WaveformChannels waveform, audioalgo::RunIdentity completed) {
        live_target.receive(std::move(outcome), std::move(waveform), std::move(completed));
    };

    bool deletion_applied = false;
    bool completion_timed_out = false;
    bool shutdown_started = false;
    const auto message_thread = std::this_thread::get_id();
    const auto run_directory = temporary.path / "runs" / "delayed";
    {
        audioalgo::editor::EditorWorkerPool pool(1);
        REQUIRE(pool.add_job(new audioalgo::editor::EditorRunJob<DeletedNodeCompletionTarget>(
            weak_state, registry, std::move(request), identity, deliver), true));

        struct DeleteGainAndDrain final : juce::Timer {
            DeleteGainAndDrain(audioalgo::editor::EditorController& controller,
                               std::shared_ptr<RunGate> gate,
                               audioalgo::editor::EditorWorkerPool& pool,
                               DeletedNodeCompletionTarget& target,
                               std::filesystem::path run_directory,
                               bool& deletion_applied, bool& completion_timed_out,
                               bool& shutdown_started)
                : controller(controller), gate(std::move(gate)), pool(pool), target(target),
                  run_directory(std::move(run_directory)), deletion_applied(deletion_applied),
                  completion_timed_out(completion_timed_out), shutdown_started(shutdown_started),
                  started(std::chrono::steady_clock::now()) {}

            void timerCallback() override {
                if (!deletion_applied && gate->has_entered()) {
                    const auto deleted = controller.delete_node("gain");
                    deletion_applied = deleted.status == audioalgo::editor::ProjectEditStatus::applied;
                    gate->release();
                }
                if (!gate->has_entered() && std::chrono::steady_clock::now() - started > std::chrono::seconds(8)) {
                    completion_timed_out = true;
                    gate->release();
                }
                const auto has_run_artifact = std::filesystem::exists(run_directory / "complete.json");
                const auto finished = pool.get_num_jobs() == 0 && has_run_artifact && target.service_notifications == 1;
                const auto exceeded = std::chrono::steady_clock::now() - started > std::chrono::seconds(20);
                if ((!finished && !exceeded) || shutdown_started)
                    return;
                if (exceeded)
                    completion_timed_out = true;
                shutdown_started = pool.shutdown_async(5000, [](bool) {
                    juce::MessageManager::getInstance()->stopDispatchLoop();
                });
                stopTimer();
            }

            audioalgo::editor::EditorController& controller;
            std::shared_ptr<RunGate> gate;
            audioalgo::editor::EditorWorkerPool& pool;
            DeletedNodeCompletionTarget& target;
            std::filesystem::path run_directory;
            bool& deletion_applied;
            bool& completion_timed_out;
            bool& shutdown_started;
            std::chrono::steady_clock::time_point started;
        } drain(controller, run_gate, pool, *target, run_directory, deletion_applied,
                 completion_timed_out, shutdown_started);

        drain.startTimer(5);
        juce::MessageManager::getInstance()->runDispatchLoop();
        drain.stopTimer();
        pool.join_shutdown();
    }

    CHECK(run_gate->has_entered());
    CHECK(run_gate->has_released());
    CHECK_FALSE(completion_timed_out);
    CHECK(shutdown_started);
    CHECK(deletion_applied);
    CHECK(controller.revision() == identity.revision + 1);
    CHECK(controller.dirty());
    CHECK(controller.results_stale());
    CHECK_FALSE(controller.node_snapshot("gain").has_value());
    CHECK(controller.project().nodes.size() == 3);
    CHECK(controller.project().connections.size() == 2);
    CHECK(target->service_notifications == 1);
    CHECK(target->observed_identity == identity);
    REQUIRE(target->observed_outcome.has_value());
    REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(*target->observed_outcome));
    CHECK_FALSE(target->accepted);
    CHECK(target->completion_handler_invocations == 0);
    CHECK(target->deleted_node_queries == 0);
    CHECK(target->presentation.result_id == "previous-result");
    CHECK(target->presentation.output_file == std::filesystem::path("previous-output.wav"));
    CHECK(target->presentation.status == "Previous run is ready");
    CHECK(target->presentation.waveform.size() == 1);
    CHECK(target->presentation.waveform.front().front().minimum == -0.5f);
    CHECK(target->presentation.waveform.front().front().maximum == 0.5f);
    CHECK(target->presentation.playback_enabled);

    const auto& result = std::get<audioalgo::RunSuccess>(*target->observed_outcome);
    CHECK(result.identity == identity);
    CHECK(result.frames == input.frame_count());
    CHECK(result.output_sha256.size() == 64);
    CHECK(std::filesystem::exists(result.output_file));
    CHECK(std::filesystem::exists(run_directory / "run.json"));
    CHECK(std::filesystem::exists(run_directory / "complete.json"));
    const auto rendered = audioalgo::read_wav_file(result.output_file);
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(rendered));
    const auto& output = std::get<audioalgo::AudioBuffer>(rendered);
    CHECK(output.sample_rate_hz() == input.sample_rate_hz());
    CHECK(output.frame_count() == input.frame_count());
    CHECK(output.channel_count() == input.channel_count());
    const auto expected_gain = std::pow(10.0, 6.0 / 20.0);
    for (const auto frame : {std::size_t{1}, std::size_t{128}, std::size_t{512}}) {
        CHECK(output.channel(0)[frame] == Catch::Approx(input.channel(0)[frame] * expected_gain).margin(2.0e-6));
        CHECK(output.channel(1)[frame] == Catch::Approx(input.channel(1)[frame] * expected_gain).margin(2.0e-6));
    }

    if (const auto* artifact_path = std::getenv("AUDIOALGO_T09_ARTIFACT_OUTPUT");
        artifact_path != nullptr && artifact_path[0] != '\0') {
        const auto artifact_directory = std::filesystem::u8path(artifact_path);
        REQUIRE_FALSE(std::filesystem::exists(artifact_directory));
        REQUIRE(std::filesystem::create_directories(artifact_directory));
        for (const auto* filename : {"output.wav", "run.json", "complete.json"})
            CHECK(std::filesystem::copy_file(run_directory / filename, artifact_directory / filename));
    }
}

TEST_CASE("AR-09.d production RunJob publishes a real run and drops delivery after View close",
          "[architecture][native_integration][lifecycle]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "production RunJob.aaproj";
    const auto input_path = temporary.path / "input.wav";
    audioalgo::AudioBuffer input(48000, {"left", "right"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.25 * std::sin(frame * 0.017));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.18 * std::cos(frame * 0.023));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        audioalgo::write_wav_file(input_path, input, audioalgo::WavEncoding::float32)));
    const auto project = make_gain_project();
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_path, project)));

    auto registry = std::make_shared<audioalgo::NodeRegistry>();
    REQUIRE(audioalgo::register_builtin_nodes(*registry));
    auto run_gate = std::make_shared<RunGate>();
    const audioalgo::NodeDescriptor gate_descriptor{
        "test.run_gate", "1.0.0", "Test run gate",
        {{"audio_in", "audio.pcm@1", audioalgo::PortDirection::input, true},
         {"audio_out", "audio.pcm@1", audioalgo::PortDirection::output, false}},
        {}, audioalgo::NodeExecutionKind::block};
    REQUIRE(registry->register_node(gate_descriptor, [run_gate] {
        return std::make_unique<RunGateProcessor>(run_gate);
    }).registered);
    std::weak_ptr<const audioalgo::NodeRegistry> registry_lifetime = registry;

    const auto blocked_project = make_blocked_gain_project();
    const auto make_request = [&](const audioalgo::ProjectModel& run_project, std::uint64_t epoch,
                                  std::string request_id, const std::filesystem::path& output_directory) {
        const audioalgo::RunIdentity identity{run_project.project_id, epoch, std::move(request_id), run_project.revision};
        audioalgo::RunRequest request{run_project, project_path, output_directory, 127};
        request.identity = identity;
        return std::pair{std::move(request), identity};
    };
    auto [closed_request, closed_identity] = make_request(
        blocked_project, 41, "closed-view-run", temporary.path / "runs" / "closed");
    auto [live_request, live_identity] = make_request(
        project, 40, "live-view-run", temporary.path / "runs" / "live");

    auto closed_target = std::make_unique<DeliveryTarget>();
    auto live_target = std::make_unique<DeliveryTarget>();
    auto closed_state = std::make_shared<audioalgo::editor::ComponentDispatchState<DeliveryTarget>>(*closed_target);
    auto live_state = std::make_shared<audioalgo::editor::ComponentDispatchState<DeliveryTarget>>(*live_target);
    std::weak_ptr<audioalgo::editor::ComponentDispatchState<DeliveryTarget>> weak_closed_state = closed_state;
    std::weak_ptr<audioalgo::editor::ComponentDispatchState<DeliveryTarget>> weak_live_state = live_state;
    const auto deliver = [](DeliveryTarget& target, audioalgo::RunOutcome result,
                            audioalgo::editor::WaveformChannels waveform, audioalgo::RunIdentity identity) {
        target.receive(std::move(result), std::move(waveform), std::move(identity));
    };

    const auto message_thread = std::this_thread::get_id();
    bool shutdown_timed_out = true;
    bool shutdown_callback_on_message_thread = false;
    bool shutdown_started = false;
    bool closed_view_destroyed = false;
    bool live_published_closed_cancelled = false;
    bool jobs_drained = false;
    {
        audioalgo::editor::EditorWorkerPool pool(1);
        REQUIRE(pool.add_job(new audioalgo::editor::EditorRunJob<DeliveryTarget>(
            weak_live_state, registry, std::move(live_request), live_identity, deliver), true));
        auto* closed_job = new audioalgo::editor::EditorRunJob<DeliveryTarget>(
            weak_closed_state, registry, std::move(closed_request), closed_identity, deliver);
        closed_job->addListener(run_gate.get());
        REQUIRE(pool.add_job(closed_job, true));
        registry.reset();

        struct DrainAfterRuns final : juce::Timer {
            DrainAfterRuns(audioalgo::editor::EditorWorkerPool& pool,
                           std::shared_ptr<RunGate> run_gate,
                           std::shared_ptr<audioalgo::editor::ComponentDispatchState<DeliveryTarget>>& closed_state,
                           std::unique_ptr<DeliveryTarget>& closed_target,
                           std::filesystem::path closed_output,
                           std::filesystem::path live_output,
                           bool& closed_view_destroyed,
                           std::function<void()> on_drained)
                : pool(pool), run_gate(std::move(run_gate)), closed_state(closed_state), closed_target(closed_target),
                  closed_output(std::move(closed_output)), live_output(std::move(live_output)),
                  closed_view_destroyed(closed_view_destroyed), on_drained(std::move(on_drained)) {}
            void timerCallback() override {
                if (run_gate->has_entered() && !closed_view_destroyed) {
                    closed_state->invalidate();
                    closed_target.reset();
                    closed_state.reset();
                    closed_view_destroyed = true;
                    close_started_at = std::chrono::steady_clock::now();
                    on_drained();
                    return;
                }
                if (closed_view_destroyed && !run_gate->has_released() &&
                    std::chrono::steady_clock::now() - close_started_at >= std::chrono::milliseconds(100))
                    run_gate->release();
                if (pool.get_num_jobs() != 0 ||
                    std::filesystem::exists(closed_output) ||
                    !std::filesystem::exists(live_output / "complete.json"))
                    return;
                stopTimer();
            }
            audioalgo::editor::EditorWorkerPool& pool;
            std::shared_ptr<RunGate> run_gate;
            std::shared_ptr<audioalgo::editor::ComponentDispatchState<DeliveryTarget>>& closed_state;
            std::unique_ptr<DeliveryTarget>& closed_target;
            std::filesystem::path closed_output;
            std::filesystem::path live_output;
            bool& closed_view_destroyed;
            std::function<void()> on_drained;
            std::chrono::steady_clock::time_point close_started_at{};
        };
        DrainAfterRuns drain_after_runs(pool, run_gate, closed_state, closed_target,
                                        temporary.path / "runs" / "closed", temporary.path / "runs" / "live",
                                        closed_view_destroyed, [&] {
            shutdown_started = pool.shutdown_async(5000, [&](bool first_wait_timed_out) {
                shutdown_timed_out = first_wait_timed_out;
                shutdown_callback_on_message_thread = std::this_thread::get_id() == message_thread;
                juce::MessageManager::getInstance()->stopDispatchLoop();
            });
        });
        drain_after_runs.startTimer(5);
        juce::MessageManager::getInstance()->runDispatchLoop();
        drain_after_runs.stopTimer();
        pool.join_shutdown();
        jobs_drained = pool.get_num_jobs() == 0;
        live_published_closed_cancelled = !std::filesystem::exists(temporary.path / "runs" / "closed") &&
                         std::filesystem::exists(temporary.path / "runs" / "live" / "complete.json");
    }

    // P2 cancellation now honors the shutdown exit signal. The independent live
    // branch below retains all of its original numerical output assertions.
    CHECK(live_published_closed_cancelled);
    CHECK(run_gate->exit_signal.load());
    CHECK(run_gate->completed_blocks.load() == 1);
    CHECK(jobs_drained);
    CHECK(shutdown_started);
    CHECK(run_gate->has_entered());
    CHECK(run_gate->has_released());
    CHECK(closed_view_destroyed);
    CHECK_FALSE(shutdown_timed_out);
    CHECK(shutdown_callback_on_message_thread);
    CHECK(weak_closed_state.expired());
    CHECK(registry_lifetime.expired());
    CHECK_FALSE(std::filesystem::exists(temporary.path / "runs" / "closed" / "run.json"));
    CHECK_FALSE(std::filesystem::exists(temporary.path / "runs" / "closed" / "complete.json"));
    CHECK(live_target->deliveries == 1);
    CHECK(live_target->delivery_thread == message_thread);
    REQUIRE(live_target->outcome.has_value());
    REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(*live_target->outcome));
    const auto& result = std::get<audioalgo::RunSuccess>(*live_target->outcome);
    CHECK(result.identity == live_identity);
    CHECK(result.run_id == live_identity.request_id);
    CHECK(result.frames == input.frame_count());
    CHECK(result.output_sha256.size() == 64);
    CHECK(live_target->identity == live_identity);
    REQUIRE(live_target->waveform.size() == 2);
    REQUIRE(live_target->waveform[0].size() == input.frame_count());
    CHECK(live_target->waveform[0][0].minimum == Catch::Approx(
        input.channel(0)[0] * static_cast<float>(std::pow(10.0, 6.0 / 20.0))).margin(2.0e-6));

    const auto rendered = audioalgo::read_wav_file(result.output_file);
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(rendered));
    const auto& output = std::get<audioalgo::AudioBuffer>(rendered);
    REQUIRE(output.frame_count() == input.frame_count());
    REQUIRE(output.channel_count() == input.channel_count());
    const auto gain = std::pow(10.0, 6.0 / 20.0);
    for (const auto frame : {std::size_t{1}, std::size_t{128}, std::size_t{512}}) {
        CHECK(output.channel(0)[frame] == Catch::Approx(input.channel(0)[frame] * gain).margin(2.0e-6));
        CHECK(output.channel(1)[frame] == Catch::Approx(input.channel(1)[frame] * gain).margin(2.0e-6));
    }
    CHECK(std::filesystem::exists(result.manifest_file));
    CHECK(std::filesystem::exists(result.complete_file));
    CHECK_FALSE(std::filesystem::exists(temporary.path / "runs" / "closed" / "output.wav"));
    CHECK_FALSE(std::filesystem::exists(temporary.path / "runs" / "closed"));

    if (const auto* artifact_path = std::getenv("AUDIOALGO_T05E_ARTIFACT_OUTPUT");
        artifact_path != nullptr && artifact_path[0] != '\0') {
        const auto artifact_directory = std::filesystem::u8path(artifact_path);
        REQUIRE_FALSE(std::filesystem::exists(artifact_directory));
        REQUIRE(std::filesystem::create_directories(artifact_directory));
        for (const auto* run_name : {"live"}) {
            const auto source = temporary.path / "runs" / run_name;
            const auto destination = artifact_directory / run_name;
            REQUIRE(std::filesystem::create_directories(destination));
            for (const auto* filename : {"output.wav", "run.json", "complete.json"})
                CHECK(std::filesystem::copy_file(source / filename, destination / filename));
        }
        juce::DynamicObject::Ptr observation = new juce::DynamicObject;
        observation->setProperty("closed_run_id", juce::String(closed_identity.request_id));
        observation->setProperty("shutdown_exit_signal_observed", run_gate->exit_signal.load());
        observation->setProperty("completed_blocks", static_cast<int>(run_gate->completed_blocks.load()));
        observation->setProperty("closed_directory_exists", std::filesystem::exists(temporary.path / "runs" / "closed"));
        CHECK(juce::File((artifact_directory / "closed-cancellation.json").string()).replaceWithText(juce::JSON::toString(juce::var(observation.get()))));
    }
}
