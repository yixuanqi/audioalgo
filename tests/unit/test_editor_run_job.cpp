#include "../../apps/editor/EditorRunJob.h"
#include "../../apps/editor/EditorRunCompletion.h"
#include "../../apps/editor/EditorWorkerPool.h"
#include "../../apps/editor/EditorWorkspace.h"
#include "audio_editor_application/EditorController.h"
#include "audio_editor_application/AudioPreviewService.h"
#include "audio_editor_application/ProjectDocumentService.h"
#include "audio_node_authoring/Draft.h"
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

void check_gain_output_samples(const audioalgo::AudioBuffer& input,
                               const audioalgo::AudioBuffer& output,
                               double gain) {
    REQUIRE(output.sample_rate_hz() == 48000);
    REQUIRE(output.channel_count() == 2);
    REQUIRE(output.frame_count() == 513);
    REQUIRE(output.channel_ids() == input.channel_ids());
    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (std::size_t frame = 0; frame < 513; ++frame) {
            const auto expected = static_cast<double>(input.channel(channel)[frame]) * gain;
            const auto actual = static_cast<double>(output.channel(channel)[frame]);
            CHECK(std::abs(actual - expected) <= 2.0e-6 + 1.0e-5 * std::abs(expected));
        }
    }
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

class EditorTestDocumentService final : public audioalgo::ProjectDocumentService {
public:
    audioalgo::ProjectDocumentLoadOutcome load(const std::filesystem::path&) const override {
        return audioalgo::Diagnostic{"test_document_unconfigured", ""};
    }
    audioalgo::ProjectDocumentSaveOutcome save(const std::filesystem::path&,
                                                const audioalgo::ProjectModel&) const override {
        return std::monostate{};
    }
};

class EditorTestDraftStorage final : public audioalgo::authoring::DraftStorage {
public:
    audioalgo::authoring::SaveResult save(const audioalgo::authoring::Draft&) const override {
        return audioalgo::Diagnostic{"test_draft_unconfigured", ""};
    }
    audioalgo::authoring::LoadResult load(const std::string&) const override {
        return audioalgo::Diagnostic{"test_draft_unconfigured", ""};
    }
    audioalgo::authoring::ListResult list() const override {
        return audioalgo::Diagnostic{"test_draft_unconfigured", ""};
    }
};

class EditorTestPreviewService final : public audioalgo::AudioPreviewService {
public:
    audioalgo::AudioPreviewOutcome play(const std::filesystem::path& path) override {
        played_path = path;
        playing = true;
        return std::monostate{};
    }
    void pause() override { playing = false; }
    void stop() override { playing = false; played_path.clear(); }
    void shutdown() override { stop(); shut_down = true; }
    bool is_playing() const noexcept override { return playing; }
    double position_seconds() const noexcept override { return 0.0; }
    std::filesystem::path played_path;
    bool playing = false;
    bool shut_down = false;
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

namespace audioalgo::editor {
struct EditorComponentTestAccess {
    static void adopt_project(EditorComponent& component, ProjectModel project,
                              const std::filesystem::path& path, std::uint64_t epoch) {
        component.controller_.adopt_project(std::move(project));
        component.project_path_ = path;
        component.project_epoch_ = epoch;
    }
    static void seed_current_result(EditorComponent& component, std::filesystem::path path,
                                    const RunIdentity& identity, const juce::String& status) {
        component.run_reception_.begin(identity);
        component.output_file_ = std::move(path);
        WaveformChannels waveform(1);
        waveform[0] = {{-0.25f, 0.5f}, {-0.5f, 0.75f}, {-0.125f, 0.25f}};
        component.waveform_.set_waveform(std::move(waveform));
        component.controller_.mark_result_current();
        component.play_button_.setEnabled(true);
        component.set_status(status, false);
    }
    static ProjectEditOutcome disconnect(EditorComponent& component, const Connection& edge) {
        auto result = component.controller_.disconnect(edge);
        if (result.status == ProjectEditStatus::applied)
            component.mark_changed();
        return result;
    }
    static ProjectEditOutcome connect(EditorComponent& component, const Connection& edge) {
        auto result = component.controller_.connect(edge);
        if (result.status == ProjectEditStatus::applied)
            component.mark_changed();
        return result;
    }
    static void finish(EditorComponent& component, RunOutcome outcome,
                       WaveformChannels waveform, RunIdentity identity) {
        component.run_finished(std::move(outcome), std::move(waveform), std::move(identity));
    }
    static void receive_log(EditorComponent& component, NodeLogEvent event) {
        component.run_log_received(std::move(event));
    }
    static void begin_new_project(EditorComponent& component, ProjectModel project,
                                  std::uint64_t epoch) {
        component.advance_project_epoch();
        component.controller_.adopt_project(std::move(project));
        component.project_epoch_ = epoch;
    }
    static const ProjectModel& project(const EditorComponent& component) { return component.project_; }
    static std::uint64_t epoch(const EditorComponent& component) { return component.project_epoch_; }
    static std::shared_ptr<ComponentDispatchState<EditorComponent>> dispatch_state(EditorComponent& component) {
        return component.completion_dispatch_state_;
    }
    static bool results_stale(const EditorComponent& component) { return component.controller_.results_stale(); }
    static bool run_is_active(const EditorComponent& component) { return component.run_reception_.active().has_value(); }
    static std::filesystem::path output_file(const EditorComponent& component) { return component.output_file_; }
    static bool play_enabled(const EditorComponent& component) { return component.play_button_.isEnabled(); }
    static bool preview_is_playing(const EditorComponent& component) { return component.preview_service_->is_playing(); }
    static std::pair<double, double> waveform_frames(const EditorComponent& component) {
        return component.waveform_.visible_frames();
    }
    static void set_status(EditorComponent& component, const juce::String& status) {
        component.set_status(status, false);
    }
    static void invoke_run(EditorComponent& component) { component.run_project(); }
    static void invoke_undo(EditorComponent& component) { component.undo_topology(); }
    static std::uint64_t revision(const EditorComponent& component) { return component.controller_.revision(); }
    static std::size_t connection_count(const EditorComponent& component) {
        return component.controller_.project().connections.size();
    }
    static juce::String status(const EditorComponent& component) { return component.status_.status_text(); }
    static void append_log(EditorComponent& component, const NodeLogEvent& event) { component.run_logs_.append(event); }
    static juce::String log_text(const EditorComponent& component) {
        if (component.run_logs_.getNumChildComponents() == 0)
            return {};
        const auto* text = dynamic_cast<const RunLogTextView*>(component.run_logs_.getChildComponent(0));
        return text == nullptr ? juce::String{} : text->getText();
    }
};
} // namespace audioalgo::editor

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

TEST_CASE("PC-03.c queued log delivery after View invalidation releases payloads without touching a replacement",
          "[PC-03.c][lifecycle]") {
    juce::ScopedJuceInitialiser_GUI gui;
    using State = audioalgo::editor::ComponentDispatchState<DeliveryTarget>;
    auto old_target = std::make_unique<DeliveryTarget>();
    auto state = std::make_shared<State>(*old_target);
    std::weak_ptr<State> old_state = state;
    auto payload = std::make_shared<audioalgo::NodeLogEvent>();
    payload->identity = {"old-project", 1, "old-request", 1}; payload->text = "late log";
    std::weak_ptr<audioalgo::NodeLogEvent> weak_payload = payload;
    int old_deliveries = 0;
    bool posted = false;
    std::thread worker([&] {
        posted = audioalgo::editor::dispatch_to_live_component(old_state,
            [payload, &old_deliveries](DeliveryTarget&) { ++old_deliveries; });
    });
    worker.join(); REQUIRE(posted);
    // The worker has queued an actual production dispatch, but the message
    // thread has not delivered it. Close the old View at this exact boundary.
    state->invalidate(); old_target.reset(); state.reset(); payload.reset();
    CHECK_FALSE(old_state.expired()); CHECK_FALSE(weak_payload.expired());
    auto replacement = std::make_unique<DeliveryTarget>();
    auto replacement_state = std::make_shared<State>(*replacement);
    bool drained = false;
    REQUIRE(audioalgo::editor::dispatch_to_live_component(std::weak_ptr<State>(replacement_state),
        [&](DeliveryTarget& live) { ++live.deliveries; drained = true; }));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!drained && std::chrono::steady_clock::now() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    REQUIRE(drained);
    CHECK(old_deliveries == 0); CHECK(replacement->deliveries == 1);
    CHECK(old_state.expired()); CHECK(weak_payload.expired());
    replacement_state->invalidate();
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
    REQUIRE(live_target->waveform.detail);
    CHECK(live_target->waveform.detail->frames == input.frame_count());
    CHECK(live_target->waveform.detail->sample_rate_hz == input.sample_rate_hz());
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

TEST_CASE("NCE-08.b Topology edits invalidate displayed results and stale run messages",
          "[connection_library][NCE-08][native_integration]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto project_path = temporary.path / "same revision topology edit.aaproj";
    const auto input_path = temporary.path / "input.wav";
    audioalgo::AudioBuffer input(48000, {"left", "right"}, 257);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.23 * std::sin(frame * 0.031));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.14 * std::cos(frame * 0.047));
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

    auto project = make_blocked_gain_project();
    project.project_id = "nce08b-project-A";
    REQUIRE(audioalgo::validate_project(project, *registry).empty());
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_path, project)));
    const audioalgo::RunIdentity old_identity{project.project_id, 71, "late-nce08b-run", project.revision};
    auto preview = std::make_shared<EditorTestPreviewService>();
    audioalgo::editor::EditorComponent component(
        registry, std::make_shared<EditorTestDocumentService>(),
        std::make_shared<audioalgo::ProjectResourceService>(), preview,
        std::make_shared<EditorTestDraftStorage>(), false);
    audioalgo::editor::EditorComponentTestAccess::adopt_project(component, project, project_path, 71);
    audioalgo::editor::EditorComponentTestAccess::seed_current_result(
        component, temporary.path / "previous-valid-result.wav", old_identity, "Previous run is ready");
    REQUIRE(std::holds_alternative<std::monostate>(preview->play(temporary.path / "previous-valid-result.wav")));
    REQUIRE(preview->is_playing());
    const auto previous_revision = project.revision;

    auto run_project = project;
    audioalgo::RunRequest request{run_project, project_path, temporary.path / "runs" / "late", 127};
    request.identity = old_identity;
    std::optional<audioalgo::RunSuccess> late_success;
    audioalgo::editor::WaveformChannels late_waveform;
    bool completion_dispatched = false;
    audioalgo::editor::EditorWorkerPool pool(1);
    REQUIRE(pool.add_job(new audioalgo::editor::EditorRunJob<audioalgo::editor::EditorComponent>(
        std::weak_ptr<audioalgo::editor::ComponentDispatchState<audioalgo::editor::EditorComponent>>(
            audioalgo::editor::EditorComponentTestAccess::dispatch_state(component)),
        registry, std::move(request), old_identity, std::make_shared<audioalgo::ProjectResourceService>(),
        [&](audioalgo::editor::EditorComponent& target, audioalgo::RunOutcome outcome,
            audioalgo::editor::WaveformChannels waveform, audioalgo::RunIdentity identity) {
            if (const auto* success = std::get_if<audioalgo::RunSuccess>(&outcome))
                late_success = *success;
            late_waveform = waveform;
            audioalgo::editor::EditorComponentTestAccess::finish(
                target, std::move(outcome), std::move(waveform), std::move(identity));
            completion_dispatched = true;
        },
        [&](audioalgo::editor::EditorComponent& target, audioalgo::NodeLogEvent event) {
            audioalgo::editor::EditorComponentTestAccess::receive_log(target, std::move(event));
        }, {}), true));

    const auto gate_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!run_gate->has_entered() && std::chrono::steady_clock::now() < gate_deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    REQUIRE(run_gate->has_entered());

    const audioalgo::Connection edge{"gate", "audio_out", "output", "audio"};
    const auto disconnected = audioalgo::editor::EditorComponentTestAccess::disconnect(component, edge);
    REQUIRE(disconnected.status == audioalgo::editor::ProjectEditStatus::applied);
    CHECK(audioalgo::editor::EditorComponentTestAccess::results_stale(component));
    CHECK(audioalgo::editor::EditorComponentTestAccess::output_file(component).empty());
    CHECK(audioalgo::editor::EditorComponentTestAccess::waveform_frames(component).second == 0.0);
    CHECK_FALSE(audioalgo::editor::EditorComponentTestAccess::play_enabled(component));
    CHECK_FALSE(audioalgo::editor::EditorComponentTestAccess::preview_is_playing(component));
    CHECK_FALSE(audioalgo::editor::EditorComponentTestAccess::run_is_active(component));

    const auto reconnected = audioalgo::editor::EditorComponentTestAccess::connect(component, edge);
    REQUIRE(reconnected.status == audioalgo::editor::ProjectEditStatus::applied);
    CHECK(audioalgo::editor::EditorComponentTestAccess::project(component).revision == previous_revision + 2);
    CHECK(audioalgo::editor::EditorComponentTestAccess::results_stale(component));
    CHECK(audioalgo::editor::EditorComponentTestAccess::output_file(component).empty());

    run_gate->release();
    const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (!completion_dispatched && std::chrono::steady_clock::now() < completion_deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    REQUIRE(completion_dispatched);
    while (pool.get_num_jobs() != 0 && std::chrono::steady_clock::now() < completion_deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    CHECK(pool.get_num_jobs() == 0);
    REQUIRE(late_success.has_value());
    CHECK(std::filesystem::exists(late_success->complete_file));
    const auto rendered = audioalgo::read_wav_file(late_success->output_file);
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(rendered));
    const auto& output = std::get<audioalgo::AudioBuffer>(rendered);
    const auto gain = std::pow(10.0, 6.0 / 20.0);
    for (const auto frame : {std::size_t{1}, std::size_t{128}, std::size_t{256}}) {
        CHECK(output.channel(0)[frame] == Catch::Approx(input.channel(0)[frame] * gain).margin(2.0e-6));
        CHECK(output.channel(1)[frame] == Catch::Approx(input.channel(1)[frame] * gain).margin(2.0e-6));
    }
    CHECK(audioalgo::editor::EditorComponentTestAccess::output_file(component).empty());
    CHECK(audioalgo::editor::EditorComponentTestAccess::waveform_frames(component).second == 0.0);

    auto replacement_project = make_gain_project();
    replacement_project.project_id = "nce08b-project-B";
    replacement_project.revision = previous_revision;
    audioalgo::editor::EditorComponentTestAccess::begin_new_project(component, replacement_project, 72);
    CHECK(audioalgo::editor::EditorComponentTestAccess::project(component).revision == old_identity.revision);
    CHECK(audioalgo::editor::EditorComponentTestAccess::project(component).project_id != old_identity.project_id);
    const audioalgo::RunIdentity current_identity{replacement_project.project_id, 72, "current-project-run",
                                                  replacement_project.revision};
    audioalgo::editor::EditorComponentTestAccess::seed_current_result(
        component, temporary.path / "current-project-output.wav", current_identity, "Current project is ready");
    audioalgo::NodeLogEvent current_log;
    current_log.identity = current_identity;
    current_log.node_id = "gain";
    current_log.type_id = "builtin.gain";
    current_log.source = "stdout";
    current_log.level = "info";
    current_log.text = "current-project-log";
    audioalgo::editor::EditorComponentTestAccess::append_log(component, current_log);
    const auto current_logs = audioalgo::editor::EditorComponentTestAccess::log_text(component);
    const auto current_status = audioalgo::editor::EditorComponentTestAccess::status(component);
    const auto current_output = audioalgo::editor::EditorComponentTestAccess::output_file(component);

    audioalgo::editor::EditorComponentTestAccess::finish(
        component, *late_success, late_waveform, old_identity);
    audioalgo::editor::EditorComponentTestAccess::finish(
        component, audioalgo::Diagnostic{"late_run_failure", "must not replace the current project status"}, {},
        old_identity);
    audioalgo::NodeLogEvent stale_log = current_log;
    stale_log.identity = old_identity;
    stale_log.text = "stale-old-project-log";
    audioalgo::editor::EditorComponentTestAccess::receive_log(component, std::move(stale_log));

    CHECK(audioalgo::editor::EditorComponentTestAccess::project(component).project_id == replacement_project.project_id);
    CHECK(audioalgo::editor::EditorComponentTestAccess::project(component).revision == old_identity.revision);
    CHECK(audioalgo::editor::EditorComponentTestAccess::output_file(component) == current_output);
    CHECK(audioalgo::editor::EditorComponentTestAccess::waveform_frames(component).second == 3.0);
    CHECK(audioalgo::editor::EditorComponentTestAccess::play_enabled(component));
    CHECK(audioalgo::editor::EditorComponentTestAccess::status(component) == current_status);
    CHECK(audioalgo::editor::EditorComponentTestAccess::log_text(component) == current_logs);
    CHECK(audioalgo::editor::EditorComponentTestAccess::log_text(component).contains("current-project-log"));
    CHECK_FALSE(audioalgo::editor::EditorComponentTestAccess::log_text(component).contains("stale-old-project-log"));
}

TEST_CASE("NCE-08.c Repaired connection gets a fresh Run and Undo cannot reuse its result",
          "[connection_library][NCE-08][native_integration]") {
    juce::ScopedJuceInitialiser_GUI juce_message_environment;
    TemporaryDirectory temporary;
    const auto input_path = temporary.path / "input.wav";
    const auto project_path = temporary.path / "nce08c-repair-project.aaproj";
    audioalgo::AudioBuffer input(48000, {"ch0", "ch1"}, 513);
    for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
        input.mutable_channel(0)[frame] = static_cast<float>(0.27 * std::sin(frame * 0.027));
        input.mutable_channel(1)[frame] = static_cast<float>(-0.19 * std::cos(frame * 0.043));
    }
    REQUIRE(std::holds_alternative<audioalgo::WavWriteSummary>(
        audioalgo::write_wav_file(input_path, input, audioalgo::WavEncoding::float32)));

    auto project = make_gain_project();
    project.project_id = "nce08c-repair-run-project";
    project.name = "Repair, Run, then Undo";
    REQUIRE(std::holds_alternative<std::monostate>(audioalgo::save_project_file(project_path, project)));
    auto registry = std::make_shared<audioalgo::NodeRegistry>();
    REQUIRE(audioalgo::register_builtin_nodes(*registry));
    REQUIRE(audioalgo::validate_project(project, *registry).empty());

    auto preview = std::make_shared<EditorTestPreviewService>();
    audioalgo::editor::EditorComponent component(
        registry, std::make_shared<EditorTestDocumentService>(),
        std::make_shared<audioalgo::ProjectResourceService>(), preview,
        std::make_shared<EditorTestDraftStorage>(), false);
    audioalgo::editor::EditorComponentTestAccess::adopt_project(component, project, project_path, 83);

    const audioalgo::Connection output_edge{"gain", "audio_out", "output", "audio"};
    const auto initial_revision = audioalgo::editor::EditorComponentTestAccess::revision(component);
    const auto disconnected = audioalgo::editor::EditorComponentTestAccess::disconnect(component, output_edge);
    REQUIRE(disconnected.status == audioalgo::editor::ProjectEditStatus::applied);
    const auto disconnected_revision = audioalgo::editor::EditorComponentTestAccess::revision(component);
    REQUIRE(disconnected_revision == initial_revision + 1);
    const auto broken_diagnostics = audioalgo::validate_project(
        audioalgo::editor::EditorComponentTestAccess::project(component), *registry);
    REQUIRE_FALSE(broken_diagnostics.empty());
    CHECK(broken_diagnostics.front().code == "input_required");

    audioalgo::editor::EditorComponentTestAccess::invoke_run(component);
    INFO("status after rejected broken-graph Run: " <<
         audioalgo::editor::EditorComponentTestAccess::status(component).toStdString());
    CHECK(audioalgo::editor::EditorComponentTestAccess::status(component).contains("input_required"));
    CHECK(audioalgo::editor::EditorComponentTestAccess::output_file(component).empty());
    CHECK_FALSE(audioalgo::editor::EditorComponentTestAccess::run_is_active(component));
    CHECK_FALSE(std::filesystem::exists(temporary.path / "runs"));

    const auto reconnected = audioalgo::editor::EditorComponentTestAccess::connect(component, output_edge);
    REQUIRE(reconnected.status == audioalgo::editor::ProjectEditStatus::applied);
    const auto repaired_revision = audioalgo::editor::EditorComponentTestAccess::revision(component);
    REQUIRE(repaired_revision == disconnected_revision + 1);
    CHECK(audioalgo::editor::EditorComponentTestAccess::connection_count(component) == 2);
    CHECK(audioalgo::editor::EditorComponentTestAccess::results_stale(component));

    audioalgo::editor::EditorComponentTestAccess::invoke_run(component);
    const auto run_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < run_deadline &&
           (audioalgo::editor::EditorComponentTestAccess::output_file(component).empty() ||
            component.background_job_count() != 0))
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    REQUIRE(component.background_job_count() == 0);
    const auto successful_output = audioalgo::editor::EditorComponentTestAccess::output_file(component);
    REQUIRE_FALSE(successful_output.empty());
    REQUIRE(std::filesystem::exists(successful_output));
    const auto run_directory = successful_output.parent_path();
    REQUIRE(std::filesystem::exists(run_directory / "run.json"));
    REQUIRE(std::filesystem::exists(run_directory / "complete.json"));
    CHECK_FALSE(audioalgo::editor::EditorComponentTestAccess::results_stale(component));
    CHECK(audioalgo::editor::EditorComponentTestAccess::play_enabled(component));
    CHECK(audioalgo::editor::EditorComponentTestAccess::status(component).contains("Completed run"));

    const auto manifest_file = juce::File(juce::String::fromUTF8((run_directory / "run.json").generic_string().c_str()));
    const auto manifest_var = juce::JSON::parse(manifest_file.loadFileAsString());
    REQUIRE(manifest_var.isObject());
    const auto run_id = manifest_var["run_id"].toString().toStdString();
    const auto output_hash = manifest_var["output_sha256"].toString().toStdString();
    REQUIRE_FALSE(run_id.empty());
    CHECK(manifest_var["project_id"].toString() == juce::String::fromUTF8(project.project_id.c_str()));
    CHECK(manifest_var["project_revision"].toString().getLargeIntValue() ==
          static_cast<juce::int64>(repaired_revision));
    CHECK(manifest_var["request_id"].toString() == juce::String::fromUTF8(run_id.c_str()));
    CHECK(manifest_var["sample_rate_hz"].toString().getIntValue() == 48000);
    CHECK(manifest_var["channels"].toString().getIntValue() == 2);
    CHECK(manifest_var["frames"].toString().getLargeIntValue() == 513);

    const auto rendered_result = audioalgo::read_wav_file(successful_output);
    REQUIRE(std::holds_alternative<audioalgo::AudioBuffer>(rendered_result));
    const auto& rendered = std::get<audioalgo::AudioBuffer>(rendered_result);
    const auto gain = std::pow(10.0, 6.0 / 20.0);
    check_gain_output_samples(input, rendered, gain);

    audioalgo::editor::EditorComponentTestAccess::invoke_undo(component);
    const auto undone_revision = audioalgo::editor::EditorComponentTestAccess::revision(component);
    CHECK(undone_revision == repaired_revision + 1);
    CHECK(audioalgo::editor::EditorComponentTestAccess::connection_count(component) == 1);
    CHECK(audioalgo::editor::EditorComponentTestAccess::results_stale(component));
    CHECK(audioalgo::editor::EditorComponentTestAccess::output_file(component).empty());
    CHECK_FALSE(audioalgo::editor::EditorComponentTestAccess::play_enabled(component));
    CHECK_FALSE(audioalgo::editor::EditorComponentTestAccess::preview_is_playing(component));
    CHECK_FALSE(audioalgo::editor::EditorComponentTestAccess::run_is_active(component));

    audioalgo::editor::EditorComponentTestAccess::invoke_run(component);
    CHECK(audioalgo::editor::EditorComponentTestAccess::status(component).contains("input_required"));
    CHECK(audioalgo::editor::EditorComponentTestAccess::output_file(component).empty());
    CHECK_FALSE(audioalgo::editor::EditorComponentTestAccess::run_is_active(component));
    std::size_t completed_runs = 0;
    if (std::filesystem::exists(temporary.path / "runs"))
        for (const auto& entry : std::filesystem::directory_iterator(temporary.path / "runs"))
            if (entry.is_directory() && std::filesystem::exists(entry.path() / "complete.json"))
                ++completed_runs;
    CHECK(completed_runs == 1);
    CHECK(std::filesystem::exists(successful_output));
    CHECK_FALSE(audioalgo::editor::EditorComponentTestAccess::status(component).contains(run_id));

    if (const auto* location = std::getenv("AUDIOALGO_NCE08C_ARTIFACT_OUTPUT"); location && *location) {
        const auto artifact_root = std::filesystem::u8path(location);
        REQUIRE_FALSE(std::filesystem::exists(artifact_root));
        std::filesystem::create_directories(artifact_root);
        REQUIRE(std::filesystem::copy_file(input_path, artifact_root / "input.wav"));
        REQUIRE(std::filesystem::copy_file(project_path, artifact_root / "original-project.aaproj"));
        std::filesystem::copy(run_directory, artifact_root / "successful-run",
                              std::filesystem::copy_options::recursive);
        juce::DynamicObject::Ptr receipt = new juce::DynamicObject();
        receipt->setProperty("project_id", juce::String::fromUTF8(project.project_id.c_str()));
        receipt->setProperty("initial_revision", static_cast<juce::int64>(initial_revision));
        receipt->setProperty("disconnected_revision", static_cast<juce::int64>(disconnected_revision));
        receipt->setProperty("repaired_revision", static_cast<juce::int64>(repaired_revision));
        receipt->setProperty("undone_revision", static_cast<juce::int64>(undone_revision));
        receipt->setProperty("run_id", juce::String::fromUTF8(run_id.c_str()));
        receipt->setProperty("output_sha256", juce::String::fromUTF8(output_hash.c_str()));
        receipt->setProperty("undo_current_run_active", audioalgo::editor::EditorComponentTestAccess::run_is_active(component));
        receipt->setProperty("undo_results_stale", audioalgo::editor::EditorComponentTestAccess::results_stale(component));
        receipt->setProperty("undo_displayed_output_empty", audioalgo::editor::EditorComponentTestAccess::output_file(component).empty());
        receipt->setProperty("undo_play_enabled", audioalgo::editor::EditorComponentTestAccess::play_enabled(component));
        receipt->setProperty("post_undo_run_status", audioalgo::editor::EditorComponentTestAccess::status(component));
        const auto receipt_file = juce::File(juce::String::fromUTF8((artifact_root / "receipt.json").generic_string().c_str()));
        REQUIRE(receipt_file.replaceWithText(juce::JSON::toString(juce::var(receipt.get()), true)));
    }
}
