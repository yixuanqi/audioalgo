#pragma once
#include "EditorNodeSummary.h"
#include "EditorNodeInspector.h"
#include "EditorGeometry.h"
#include "EditorComponentLayout.h"
#include "EditorText.h"
#include "EditorNodeCreation.h"
#include "EditorNodeDragDrop.h"
#include "EditorTopologyHistory.h"
#include "EditorTopologyCommands.h"
#include "EditorKeyboardShortcuts.h"
#include "EditorCompletionDispatch.h"
#include "EditorWorkerPool.h"
#include "EditorWorkJob.h"
#include "EditorRunCompletion.h"
#include "EditorRunJob.h"
#include "EditorRunQueue.h"
#include "EditorNodeParameterBinding.h"
#include "EditorParameterPanel.h"
#include "audio_node_sdk/NodeSDK.h"
#include "audio_io/WavFile.h"
#include "audio_project/ProjectFileIO.h"
#include "audio_project/ProjectModel.h"
#include "audio_run_service/RunService.h"
#include "audio_editor_application/EditorController.h"
#include "audio_editor_application/EditorProjectDocuments.h"
#include "audio_editor_application/ProjectDocumentService.h"
#include "audio_editor_application/AudioPreviewService.h"
#include "audio_editor_application/RunReception.h"
#include "audio_editor_application/AsyncCompletionSlot.h"


#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "EditorUiSupport.h"
#include "EditorGraphCanvas.h"
#include "EditorNodeLibrary.h"
#include "EditorWaveformView.h"
#include "EditorRunStatusPanel.h"
namespace audioalgo::editor {
class EditorComponent final : public juce::Component, public juce::DragAndDropContainer {
public:
    EditorComponent(std::shared_ptr<audioalgo::NodeRegistry> descriptors,
                    std::shared_ptr<const audioalgo::ProjectDocumentService> document_service,
                    std::shared_ptr<const audioalgo::ProjectResourceService> project_resources,
                    std::shared_ptr<audioalgo::AudioPreviewService> preview_service)
        : document_service_(std::move(document_service)), project_resources_(std::move(project_resources)),
          preview_service_(std::move(preview_service)), descriptors_owner_(std::move(descriptors)), controller_(descriptors_), project_(controller_.project()),
          canvas_(project_, descriptors_, selection_, controller_,
                  [this](std::string id) { inspector_.show_node(id); },
                  [this] { mark_changed(); },
                  [this](std::string message) { set_status(utf8_string(message), true); },
                  [this](std::string type_id, audioalgo::editor::EditorPoint point, std::uint64_t source_epoch,
                         std::uint64_t drop_revision,
                         std::optional<audioalgo::Connection> target_connection) {
                      add_dropped_library_node(type_id, point, source_epoch, drop_revision,
                                               std::move(target_connection));
                  },
                  [](juce::Component* source) -> std::optional<std::uint64_t> {
                      const auto* button = dynamic_cast<NodeLibraryButton*>(source);
                      if (button == nullptr)
                          return std::nullopt;
                      return button->gesture_project_epoch();
                  }),
          inspector_(controller_, selection_, [this] { mark_changed(); }),
          library_(descriptors_, [this](std::string id) { add_library_node(id); }, [this] { return project_epoch_; }) {
        completion_dispatch_state_ = std::make_shared<
            audioalgo::editor::ComponentDispatchState<EditorComponent>>(*this);
        inspector_.set_descriptor_registry(descriptors_);

        for (auto* button : {&new_button_, &open_button_, &save_button_, &undo_button_, &redo_button_,
                             &run_button_, &play_button_})
            addAndMakeVisible(*button);
        for (auto* button : {&delete_button_})
            addAndMakeVisible(*button);
        addAndMakeVisible(canvas_);
        addAndMakeVisible(inspector_);
        addAndMakeVisible(waveform_);
        addAndMakeVisible(status_);
        addAndMakeVisible(library_);

        style_button(open_button_);
        style_button(save_button_);
        style_button(undo_button_);
        style_button(redo_button_);
        style_button(run_button_, signal);
        style_button(play_button_, amber);
        style_button(delete_button_, danger);
        new_button_.onClick = [this] { new_project(); };
        open_button_.onClick = [this] { open_project(); };
        save_button_.onClick = [this] { save_project(); };
        undo_button_.onClick = [this] { undo_topology(); };
        redo_button_.onClick = [this] { redo_topology(); };
        run_button_.onClick = [this] { run_project(); };
        play_button_.onClick = [this] { toggle_playback(); };
        delete_button_.onClick = [this] { canvas_.delete_selected(); };

        auto initial_project = audioalgo::ProjectModel{};
        initial_project.project_id = new_id();
        initial_project.name = "Untitled Audio Graph";
        controller_.adopt_project(std::move(initial_project));
        load_staged_demo();
        setSize(audioalgo::editor::default_editor_window_size.width,
                audioalgo::editor::default_editor_window_size.height);
        update_history_controls();
    }

    void dragOperationEnded(const juce::DragAndDropTarget::SourceDetails& details) override {
        auto* button = dynamic_cast<NodeLibraryButton*>(details.sourceComponent.get());
        if (button != nullptr)
            canvas_.finish_library_drag(button->gesture_project_epoch());
    }

    ~EditorComponent() override {
        file_chooser_completion_.close();
        completion_dispatch_state_->invalidate();
        if (!shutdown_started_) {
            advance_project_epoch();
            active_file_chooser_.reset();
            preview_service_->shutdown();
            jassert(worker_pool_.get_num_jobs() == 0);
        }
        worker_pool_.join_shutdown();
    }

    void begin_shutdown(std::function<void()> on_drained) {
        if (shutdown_started_)
            return;

        shutdown_started_ = true;
        if (run_queue_) run_queue_->close();
        completion_dispatch_state_->invalidate();
        advance_project_epoch();
        file_chooser_completion_.close();
        active_file_chooser_.reset();
        preview_service_->shutdown();
        setEnabled(false);
        set_status("Closing after background work finishes…", false);
        const auto started = worker_pool_.shutdown_async(5000,
            [on_drained = std::move(on_drained)](bool first_wait_timed_out) mutable {
                if (first_wait_timed_out)
                    juce::Logger::writeToLog("Editor worker shutdown exceeded the initial five-second wait; joined active work safely.");
                if (on_drained)
                    on_drained();
            });
        jassert(started);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(background);
        g.setColour(panel);
        g.fillRect(0, 0, getWidth(), 54);
        g.fillRect(0, 54, 164, getHeight() - 54);
        g.setColour(signal);
        g.setFont(juce::FontOptions(19.0f, juce::Font::bold));
        g.drawText("AudioAlgo", 18, 10, 132, 28, juce::Justification::centredLeft);
        g.setColour(muted);
        g.setFont(juce::FontOptions(10.5f));
        g.drawText("OFFLINE WORKBENCH", 18, 33, 132, 14, juce::Justification::centredLeft);
    }

    void resized() override {
        const auto layout = audioalgo::editor::make_editor_component_layout(getWidth(), getHeight());
        const auto set_bounds = [](juce::Component& component, const auto& bounds) {
            component.setBounds(bounds.x, bounds.y, bounds.width, bounds.height);
        };
        set_bounds(new_button_, layout.new_button);
        set_bounds(open_button_, layout.open_button);
        set_bounds(save_button_, layout.save_button);
        set_bounds(undo_button_, layout.undo_button);
        set_bounds(redo_button_, layout.redo_button);
        set_bounds(run_button_, layout.run_button);
        set_bounds(play_button_, layout.play_button);
        library_.setBounds(0, 54, 164, getHeight() - 54);
        set_bounds(delete_button_, layout.delete_button);
        set_bounds(canvas_, layout.canvas);
        set_bounds(inspector_, layout.inspector);
        set_bounds(waveform_, layout.waveform);
        set_bounds(status_, layout.status);
    }

    bool keyPressed(const juce::KeyPress& key) override {
        switch (audioalgo::editor::editor_shortcut_action(key)) {
            case audioalgo::editor::EditorShortcutAction::undo:
                undo_topology();
                return true;
            case audioalgo::editor::EditorShortcutAction::redo:
                redo_topology();
                return true;
            case audioalgo::editor::EditorShortcutAction::none:
                return false;
        }
        return false;
    }

    void run_finished(audioalgo::RunOutcome outcome, WaveformChannels waveform, audioalgo::RunIdentity identity) {
        const auto completion_applied = audioalgo::editor::apply_if_current_run(
            run_reception_, identity, project_.project_id, project_epoch_, project_.revision, [&] {
                if (const auto* error = std::get_if<audioalgo::Diagnostic>(&outcome)) {
                    if (error->code == "source_resource_missing" && !error->node_id.empty()) {
                        request_source_relink(*error);
                        return;
                    }
                    set_status(utf8_string(error->code) + ": " + utf8_string(error->message), true);
                    return;
                }
                const auto& result = std::get<audioalgo::RunSuccess>(outcome);
                output_file_ = result.output_file;
                waveform_.set_waveform(std::move(waveform));
                set_status("Completed run " + utf8_string(result.run_id) + utf8_literal(" · ") +
                           juce::String(static_cast<juce::int64>(result.frames)) + utf8_literal(" frames · ") +
                           utf8_string(result.output_sha256.substr(0, 12)), false);
                play_button_.setButtonText("Play output");
            });
        juce::ignoreUnused(completion_applied);
    }

private:
    template <typename ResultType>
    bool submit_work(std::function<ResultType()> work,
                     std::function<void(EditorComponent&, ResultType)> completion) {
        using Job = audioalgo::editor::EditorWorkJob<EditorComponent, ResultType>;
        auto* job = new Job(completion_dispatch_state_, std::move(work), std::move(completion));
        if (worker_pool_.add_job(job, true))
            return true;
        delete job;
        set_status("The editor is closing; background work was not started.", true);
        return false;
    }

    void set_status(const juce::String& message, bool is_error) {
        status_.set_status(message, is_error);
    }

    void mark_changed() {
        update_history_controls();
        canvas_.repaint();
        const auto star = project_path_.empty() ? utf8_literal(" · unsaved") :
            (controller_.dirty() ? utf8_literal(" · modified") : juce::String{});
        set_status(utf8_string(project_.name) + star, false);
    }

    void advance_project_epoch() {
        run_reception_.invalidate();
        if (run_queue_) run_queue_->cancel();
        ++project_epoch_;
        if (project_epoch_ == 0)
            ++project_epoch_;
        active_file_chooser_.reset();
        file_chooser_completion_.invalidate();
        run_after_save_ = false;
        canvas_.set_project_epoch(project_epoch_);
        canvas_.select({});
        waveform_.set_waveform({});
        output_file_.clear();
        stop_playback_resources();
        play_button_.setButtonText("Play output");
    }

    void update_history_controls() {
        undo_button_.setEnabled(controller_.history().can_undo());
        redo_button_.setEnabled(controller_.history().can_redo());
    }

    void undo_topology() {
        if (!controller_.undo())
            return;
        refresh_history_selection();
        mark_changed();
        set_status("Change undone.", false);
    }

    void redo_topology() {
        if (!controller_.redo())
            return;
        refresh_history_selection();
        mark_changed();
        set_status("Change redone.", false);
    }

    void refresh_history_selection() {
        const auto node_id = canvas_.selected_node_id();
        canvas_.select(controller_.node_snapshot(node_id) ? node_id : std::string{});
    }

    void new_project() {
        advance_project_epoch();
        auto project = audioalgo::ProjectModel{};
        project.project_id = new_id();
        project.name = "Untitled Audio Graph";
        controller_.adopt_project(std::move(project));
        project_path_.clear();
        set_status("New empty project.", false);
        update_history_controls();
    }

    std::optional<audioalgo::editor::EditorPoint> next_position(const audioalgo::NodeDescriptor& descriptor) const {
        return audioalgo::editor::find_node_placement(project_.nodes, descriptors_, descriptor,
                                                       static_cast<float>(canvas_.getWidth()),
                                                       static_cast<float>(canvas_.getHeight()),
                                                       canvas_.coordinate_transform());
    }

    void commit_library_insertion(const std::string& type_id, const std::string& node_id,
                                  const audioalgo::Connection& target,
                                  audioalgo::editor::EditorPoint position, std::uint64_t source_epoch,
                                  std::uint64_t expected_revision,
                                  const audioalgo::editor::InsertionPortChoice& choice) {
        if (source_epoch != project_epoch_ || project_.revision != expected_revision) {
            set_status("The project changed while choosing insertion ports.", true);
            return;
        }
        const auto inserted = controller_.insert_node(type_id, node_id, target, position.x, position.y, choice);
        if (inserted.status != audioalgo::editor::ProjectEditStatus::applied) {
            set_status("This node cannot be inserted on that connection.", true);
            return;
        }
        canvas_.select(node_id);
        mark_changed();
    }

    void show_library_insertion_port_choice(
        const std::string& type_id, const std::string& node_id, const audioalgo::Connection& target,
        audioalgo::editor::EditorPoint position, std::uint64_t source_epoch,
        std::vector<audioalgo::editor::InsertionPortChoice> choices) {
        juce::PopupMenu menu;
        for (std::size_t index = 0; index < choices.size(); ++index) {
            const auto label = choices[index].input_port_id + " -> " + choices[index].output_port_id;
            menu.addItem(static_cast<int>(index + 1), utf8_string(label));
        }
        const auto expected_revision = project_.revision;
        juce::Component::SafePointer<EditorComponent> safe_this(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&canvas_),
            [safe_this, type_id, node_id, target, position, source_epoch, expected_revision,
             choices = std::move(choices)](int selected_id) mutable {
                if (safe_this == nullptr || selected_id <= 0 ||
                    static_cast<std::size_t>(selected_id) > choices.size())
                    return;
                safe_this->commit_library_insertion(
                    type_id, node_id, target, position, source_epoch, expected_revision,
                    choices[static_cast<std::size_t>(selected_id - 1)]);
            });
    }

    void add_dropped_library_node(const std::string& type_id, audioalgo::editor::EditorPoint pointer_position,
                                  std::uint64_t source_epoch,
                                  std::uint64_t expected_revision,
                                  std::optional<audioalgo::Connection> target_connection) {
        const auto* descriptor = descriptors_.find(type_id);
        if (descriptor == nullptr) {
            set_status("The selected node type is no longer registered.", true);
            return;
        }
        if (source_epoch != project_epoch_ || expected_revision != project_.revision) {
            set_status("The project changed during this drag; the node was not added.", true);
            return;
        }
        const auto drop_route = audioalgo::editor::decide_library_drop_route(
            descriptor, target_connection.has_value());
        if (drop_route == audioalgo::editor::LibraryDropRoute::reject) {
            set_status("Only processing nodes with compatible ports can be inserted on a connection.", true);
            return;
        }
        if (drop_route == audioalgo::editor::LibraryDropRoute::source_chooser) {
            add_source_node(type_id, pointer_position, source_epoch);
            return;
        }
        const auto node_id = audioalgo::editor::make_unique_node_id(project_.nodes, [] { return new_id(); });
        if (!node_id) {
            set_status("Could not generate a unique node ID.", true);
            return;
        }
        if (drop_route == audioalgo::editor::LibraryDropRoute::insert_on_connection && target_connection) {
            const auto position = audioalgo::editor::library_node_origin_for_centered_drop(
                *descriptor, pointer_position, canvas_.coordinate_transform());
            auto choices = audioalgo::editor::find_library_insertion_port_choices(
                project_, descriptors_, type_id, *target_connection);
            if (choices.empty()) {
                set_status("This node cannot be inserted on that connection.", true);
                return;
            }
            if (choices.size() > 1) {
                show_library_insertion_port_choice(
                    type_id, *node_id, *target_connection, position, source_epoch, std::move(choices));
                return;
            }
            commit_library_insertion(type_id, *node_id, *target_connection, position,
                                     source_epoch, project_.revision, choices.front());
            return;
        }
        const auto position = audioalgo::editor::find_library_node_drop_position(
            project_, descriptors_, *descriptor, pointer_position,
            static_cast<float>(canvas_.getWidth()), static_cast<float>(canvas_.getHeight()),
            canvas_.coordinate_transform());
        if (!position) {
            if (!audioalgo::editor::can_add_node_of_execution_kind(project_, descriptors_, descriptor->execution_kind))
                set_status("The current run path supports one sink.", true);
            else
                set_status("No visible canvas space is available at this drop point.", true);
            return;
        }
        const auto added = controller_.add_node(type_id, *node_id, position->x, position->y);
        if (added.status != audioalgo::editor::ProjectEditStatus::applied) {
            set_status("The selected node could not be added to the project.", true);
            return;
        }
        canvas_.select(*node_id);
        mark_changed();
    }

    void add_library_node(const std::string& type_id) {
        const auto* descriptor = descriptors_.find(type_id);
        if (descriptor == nullptr) {
            set_status("The selected node type is no longer registered.", true);
            return;
        }

        if (descriptor->execution_kind == audioalgo::NodeExecutionKind::source) {
            add_source_node(type_id);
            return;
        }
        add_node(type_id);
    }

    void add_source_node(const std::string& type_id,
                         std::optional<audioalgo::editor::EditorPoint> drop_pointer = std::nullopt,
                         std::uint64_t source_epoch = 0) {
        if (source_epoch == 0)
            source_epoch = project_epoch_;
        const auto* requested = descriptors_.find(type_id);
        if (requested == nullptr || requested->execution_kind != audioalgo::NodeExecutionKind::source) {
            set_status("The selected source node is no longer available.", true);
            return;
        }
        if (source_epoch != project_epoch_) {
            set_status("The project changed during this drag; the node was not added.", true);
            return;
        }
        if (!audioalgo::editor::can_add_node_of_execution_kind(
                project_, descriptors_, audioalgo::NodeExecutionKind::source)) {
            set_status("The current run path supports one source.", true);
            return;
        }
        if (drop_pointer && !audioalgo::editor::find_library_node_drop_position(
                project_, descriptors_, *requested, *drop_pointer,
                static_cast<float>(canvas_.getWidth()), static_cast<float>(canvas_.getHeight()),
                canvas_.coordinate_transform())) {
            set_status("No visible canvas space is available at this drop point.", true);
            return;
        }
        const auto title = "Choose source for " + utf8_string(requested->display_name);
        const auto expected_session = controller_.session_identity();
        const auto expected_document_generation = controller_.document_generation();
        const auto expected_project_path = project_path_;
        launch_file_chooser(title, juce::File::getSpecialLocation(juce::File::userHomeDirectory),
                            "*.wav", juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                            [this, type_id, drop_pointer, source_epoch, expected_session,
                             expected_document_generation, expected_project_path](const std::filesystem::path& selected_file) {
            if (selected_file.empty())
                return;
            if (source_epoch != project_epoch_ || controller_.session_identity() != expected_session ||
                controller_.document_generation() != expected_document_generation ||
                project_path_ != expected_project_path) {
                set_status("The project changed while choosing a source; the node was not added.", true);
                return;
            }
            const auto* descriptor = descriptors_.find(type_id);
            if (descriptor == nullptr || descriptor->execution_kind != audioalgo::NodeExecutionKind::source) {
                set_status("The source node is no longer registered.", true);
                return;
            }
            if (!audioalgo::editor::can_add_node_of_execution_kind(
                    project_, descriptors_, audioalgo::NodeExecutionKind::source)) {
                set_status("The current run path already has a source.", true);
                return;
            }
            const auto node_id = audioalgo::editor::make_unique_node_id(project_.nodes, [] { return new_id(); });
            if (!node_id) {
                set_status("Could not generate a unique node ID.", true);
                return;
            }
            const auto resources = project_resources_;
            const auto project_path = expected_project_path;
            const auto node = *node_id;
            submit_work<audioalgo::ProjectResourceReferenceOutcome>(
                [resources, selected_file, project_path] {
                    return resources->make_reference(selected_file, project_path);
                },
                [type_id, node, drop_pointer, source_epoch, expected_session, expected_document_generation,
                 expected_project_path](EditorComponent& target, audioalgo::ProjectResourceReferenceOutcome reference) {
                    if (source_epoch != target.project_epoch_ || target.controller_.session_identity() != expected_session ||
                        target.controller_.document_generation() != expected_document_generation ||
                        target.project_path_ != expected_project_path) {
                        target.set_status("The project changed while preparing the source; the node was not added.", true);
                        return;
                    }
                    if (const auto* error = std::get_if<audioalgo::Diagnostic>(&reference)) {
                        target.set_status(utf8_string(error->code) + ": " + utf8_string(error->message), true);
                        return;
                    }
                    const auto* descriptor = target.descriptors_.find(type_id);
                    if (descriptor == nullptr || descriptor->execution_kind != audioalgo::NodeExecutionKind::source ||
                        !audioalgo::editor::can_add_node_of_execution_kind(
                            target.project_, target.descriptors_, audioalgo::NodeExecutionKind::source)) {
                        target.set_status("The source cannot be added to the current project.", true);
                        return;
                    }
                    const auto position = drop_pointer
                        ? audioalgo::editor::find_library_node_drop_position(
                              target.project_, target.descriptors_, *descriptor, *drop_pointer,
                              static_cast<float>(target.canvas_.getWidth()), static_cast<float>(target.canvas_.getHeight()),
                              target.canvas_.coordinate_transform())
                        : target.next_position(*descriptor);
                    if (!position) {
                        target.set_status(drop_pointer ? "The source could not be placed at the original drop point." :
                                           "No visible canvas space is available for another node.", true);
                        return;
                    }
                    const auto added = target.controller_.add_source_node(
                        type_id, node, std::get<std::string>(reference), position->x, position->y);
                    if (added.status != audioalgo::editor::ProjectEditStatus::applied) {
                        target.set_status("The selected source could not be added to the project.", true);
                        return;
                    }
                    target.canvas_.select(node);
                    target.mark_changed();
                    target.set_status("Source added. Connect its output to a processing node.", false);
                });
        });
    }

    void add_node(const std::string& type_id) {
        const auto* descriptor = descriptors_.find(type_id);
        if (descriptor == nullptr) {
            set_status("The selected node type is not registered.", true);
            return;
        }
        if (descriptor->execution_kind == audioalgo::NodeExecutionKind::source) {
            set_status("Choose a resource to add this source node.", true);
            return;
        }
        const auto position = next_position(*descriptor);
        if (!position) {
            set_status("No visible canvas space is available for another node.", true);
            return;
        }
        const auto node_id = audioalgo::editor::make_unique_node_id(project_.nodes, [] { return new_id(); });
        if (!node_id) {
            set_status("Could not generate a unique node ID.", true);
            return;
        }
        const auto added = controller_.add_node(type_id, *node_id, position->x, position->y);
        if (added.status != audioalgo::editor::ProjectEditStatus::applied) {
            if (!audioalgo::editor::can_add_node_of_execution_kind(
                    project_, descriptors_, descriptor->execution_kind)) {
                set_status("The current run path supports one sink.", true);
                return;
            }
            set_status("The selected node descriptor or ID is invalid.", true);
            return;
        }
        canvas_.select(*node_id);
        mark_changed();
    }

    bool save_to(const std::filesystem::path& path, bool run_after_save = false) {
        const auto expected_session = controller_.session_identity();
        const auto expected_document_generation = controller_.document_generation();
        auto snapshot = controller_.project();
        snapshot.name = path_to_utf8(path.stem());
        const auto service = document_service_;
        return submit_work<audioalgo::ProjectDocumentSaveOutcome>(
            [service, path, snapshot = std::move(snapshot)] {
                return service->save(path, snapshot);
            },
            [path, expected_session, expected_document_generation, run_after_save]
            (EditorComponent& target, audioalgo::ProjectDocumentSaveOutcome outcome) {
                const auto applied = audioalgo::EditorProjectDocuments::complete_save(
                    target.controller_, std::move(outcome), expected_session, expected_document_generation,
                    [&target, path] {
                        target.project_path_ = path;
                        (void)target.controller_.set_project_name(path_to_utf8(path.stem()));
                    });
                if (const auto* error = std::get_if<audioalgo::Diagnostic>(&applied)) {
                    if (error->code != "project_session_stale")
                        target.set_status(utf8_string(error->code) + ": " + utf8_string(error->message), true);
                    return;
                }
                target.set_status("Saved " + utf8_string(path_to_utf8(path.filename())), false);
                if (run_after_save)
                    target.run_project();
            });
    }

    void launch_file_chooser(const juce::String& title, const juce::File& initial_file,
                             const juce::String& pattern, int flags,
                             std::function<void(const std::filesystem::path&)> completion) {
        if (active_file_chooser_ != nullptr) {
            set_status("Finish the open file selection first.", true);
            return;
        }
        const auto completion_token = file_chooser_completion_.begin(std::move(completion));
        if (completion_token == 0)
            return;
        active_file_chooser_ = std::make_unique<juce::FileChooser>(title, initial_file, pattern, true, false, this);
        juce::Component::SafePointer<EditorComponent> safe_this(this);
        active_file_chooser_->launchAsync(flags, [safe_this, completion_token](const juce::FileChooser& chooser) {
            if (safe_this == nullptr)
                return;
            const auto result = chooser.getResult();
            const auto selected = result == juce::File() ? std::filesystem::path{} : path_from_juce_file(result);
            juce::MessageManager::callAsync([safe_this, completion_token, selected] {
                if (safe_this != nullptr)
                    safe_this->finish_file_chooser(completion_token, selected);
            });
        });
    }

    void finish_file_chooser(audioalgo::AsyncCompletionSlot<std::filesystem::path>::Token completion_token,
                             const std::filesystem::path& selected) {
        auto completion = file_chooser_completion_.take(completion_token);
        if (!completion)
            return;
        active_file_chooser_.reset();
        completion(selected);
    }

    void save_project(bool run_after_save = false) {
        if (!project_path_.empty()) {
            (void)save_to(project_path_, run_after_save);
            return;
        }
        run_after_save_ = run_after_save;
        const auto initial = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Untitled.aaproj");
        launch_file_chooser("Save AudioAlgo project", initial, "*.aaproj",
                            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                            [this](const std::filesystem::path& selected) {
            if (selected.empty()) {
                run_after_save_ = false;
                set_status("Save cancelled. Save the project before running.", true);
                return;
            }
            auto path = selected;
            if (path.extension() != ".aaproj")
                path += ".aaproj";
            const bool should_run = run_after_save_;
            run_after_save_ = false;
            (void)save_to(path, should_run);
        });
    }

    void open_project() {
        const auto expected_session = controller_.session_identity();
        const auto expected_document_generation = controller_.document_generation();
        launch_file_chooser("Open AudioAlgo project", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
                            "*.aaproj", juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                            [this, expected_session, expected_document_generation](const std::filesystem::path& path) {
            if (path.empty())
                return;
            if (controller_.session_identity() != expected_session ||
                controller_.document_generation() != expected_document_generation) {
                set_status("The project changed while choosing a file; the selected project was not opened.", true);
                return;
            }
            const auto service = document_service_;
            submit_work<audioalgo::ProjectDocumentLoadOutcome>(
                [service, path] { return service->load(path); },
                [path, expected_session, expected_document_generation]
                (EditorComponent& target, audioalgo::ProjectDocumentLoadOutcome outcome) {
                    const auto opened = audioalgo::EditorProjectDocuments::complete_open(
                        target.controller_, std::move(outcome), expected_session, expected_document_generation,
                        [&target] { target.advance_project_epoch(); });
                    if (const auto* error = std::get_if<audioalgo::Diagnostic>(&opened)) {
                        if (error->code != "project_session_stale")
                            target.set_status(utf8_string(error->code) + ": " + utf8_string(error->message), true);
                        return;
                    }
                    target.update_history_controls();
                    target.project_path_ = path;
                    target.set_status("Opened " + utf8_string(path_to_utf8(path.filename())), false);
                });
        });
    }

    void load_staged_demo() {
        const auto app_directory = juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory();
        const auto package_directory = path_from_juce_file(
            app_directory.getChildFile("examples").getChildFile("gain_demo"));
        const auto package_project = package_directory / "gain_demo.aaproj";
        const auto user_directory = path_from_juce_file(juce::File::getSpecialLocation(
            juce::File::userDocumentsDirectory).getChildFile("AudioAlgo").getChildFile("Examples")
            .getChildFile("gain_demo"));
        const auto path = user_directory / "gain_demo.aaproj";
        const auto expected_session = controller_.session_identity();
        const auto expected_document_generation = controller_.document_generation();
        const auto service = document_service_;
        submit_work<audioalgo::ProjectDocumentLoadOutcome>(
            [package_directory, package_project, user_directory, path, service] {
                std::error_code error;
                if (!std::filesystem::is_regular_file(package_project, error)) {
                    if (error)
                        return audioalgo::ProjectDocumentLoadOutcome{audioalgo::Diagnostic{
                            "demo_package_unavailable", error.message()}};
                    return audioalgo::ProjectDocumentLoadOutcome{audioalgo::Diagnostic{
                        "demo_package_missing", "The packaged gain demo is unavailable"}};
                }
                std::filesystem::create_directories(user_directory, error);
                if (error)
                    return audioalgo::ProjectDocumentLoadOutcome{audioalgo::Diagnostic{
                        "demo_folder_create_failed", error.message()}};
                for (const auto* filename : {"input.wav", "gain_demo.aaproj", "demo_manifest.json"}) {
                    const auto source = package_directory / filename;
                    const auto destination = user_directory / filename;
                    error.clear();
                    const bool source_exists = std::filesystem::is_regular_file(source, error);
                    if (error)
                        return audioalgo::ProjectDocumentLoadOutcome{audioalgo::Diagnostic{
                            "demo_asset_read_failed", error.message()}};
                    if (!source_exists)
                        continue;
                    error.clear();
                    const bool destination_exists = std::filesystem::exists(destination, error);
                    if (error)
                        return audioalgo::ProjectDocumentLoadOutcome{audioalgo::Diagnostic{
                            "demo_asset_check_failed", error.message()}};
                    if (destination_exists)
                        continue;
                    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error);
                    if (error)
                        return audioalgo::ProjectDocumentLoadOutcome{audioalgo::Diagnostic{
                            "demo_asset_copy_failed", error.message()}};
                }
                return service->load(path);
            },
            [path, expected_session, expected_document_generation]
            (EditorComponent& target, audioalgo::ProjectDocumentLoadOutcome outcome) {
                if (const auto* error = std::get_if<audioalgo::Diagnostic>(&outcome);
                    error != nullptr && error->code == "demo_package_missing")
                    return;
                const auto opened = audioalgo::EditorProjectDocuments::complete_open(
                    target.controller_, std::move(outcome), expected_session, expected_document_generation,
                    [&target] { target.advance_project_epoch(); });
                if (const auto* error = std::get_if<audioalgo::Diagnostic>(&opened)) {
                    if (error->code != "project_session_stale")
                        target.set_status(utf8_string(error->code) + ": " + utf8_string(error->message), true);
                    return;
                }
                target.project_path_ = path;
                target.canvas_.select("gain");
                target.update_history_controls();
                target.set_status(utf8_literal(
                    "Ready · Stereo Gain Demo · source files are in Documents/AudioAlgo/Examples"), false);
            });
    }

    void run_project() {
        if (project_path_.empty()) {
            save_project(true);
            return;
        }
        const auto diagnostics = audioalgo::validate_project(project_, descriptors_);
        if (!diagnostics.empty()) {
            set_status(utf8_string(diagnostics.front().code) + ": " +
                       utf8_string(diagnostics.front().message), true);
            return;
        }
        stop_playback_resources();
        output_file_.clear();
        waveform_.set_waveform({});
        const auto revision = project_.revision;
        const audioalgo::RunIdentity identity{project_.project_id, project_epoch_, new_id(), revision};
        run_reception_.begin(identity);
        const auto run_root = project_path_.parent_path() / "runs";
        const auto output_directory = run_root / ("run-" + new_id());
        audioalgo::RunRequest request{project_, project_path_, output_directory, 512};
        request.identity = identity;
        set_status("Rendering revision " + juce::String(static_cast<juce::int64>(revision)) + utf8_literal(" in the background…"), false);
        if (!run_queue_) run_queue_ = std::make_unique<audioalgo::editor::EditorRunQueue<EditorComponent>>(
            worker_pool_, completion_dispatch_state_, descriptors_owner_, project_resources_,
            [](EditorComponent& target, audioalgo::RunOutcome outcome,
               WaveformChannels waveform, audioalgo::RunIdentity completed_identity) {
                target.run_finished(std::move(outcome), std::move(waveform), std::move(completed_identity));
            });
        if (!run_queue_->submit(std::move(request), identity)) {
            run_reception_.invalidate();
            set_status("The editor is closing; the run was not started.", true);
        }
    }

    void request_source_relink(const audioalgo::Diagnostic& diagnostic) {
        const auto node = std::find_if(project_.nodes.begin(), project_.nodes.end(), [&](const auto& item) {
            return item.id == diagnostic.node_id;
        });
        if (node == project_.nodes.end()) {
            set_status("The missing source no longer exists in this project.", true);
            return;
        }
        const auto path_parameter = node->parameters.find("path");
        if (path_parameter == node->parameters.end() ||
            !std::holds_alternative<std::string>(path_parameter->second)) {
            set_status("The missing source has no path to replace.", true);
            return;
        }
        const auto descriptor = descriptors_.find(node->type_id);
        const auto display_name = descriptor == nullptr ? node->type_id : descriptor->display_name;
        const auto expected_session = controller_.session_identity();
        const auto expected_document_generation = controller_.document_generation();
        const auto expected_revision = project_.revision;
        const auto expected_epoch = project_epoch_;
        const auto expected_project_path = project_path_;
        const auto node_id = node->id;
        const auto missing_reference = std::get<std::string>(path_parameter->second);
        set_status("Source resource is missing. Choose a replacement file to relink this node.", true);
        const auto chooser_title = "Relink source resource for " + utf8_string(display_name);
        launch_file_chooser(chooser_title, juce::File::getSpecialLocation(juce::File::userHomeDirectory),
                            "*.wav", juce::FileBrowserComponent::openMode |
                                         juce::FileBrowserComponent::canSelectFiles,
                            [this, expected_session, expected_document_generation, expected_revision,
                             expected_epoch, expected_project_path, node_id,
                             missing_reference](const std::filesystem::path& selected_file) {
            if (selected_file.empty())
                return;
            if (controller_.session_identity() != expected_session ||
                controller_.document_generation() != expected_document_generation ||
                controller_.revision() != expected_revision || project_epoch_ != expected_epoch ||
                project_path_ != expected_project_path) {
                set_status("The project changed while choosing a replacement; no resource was relinked.", true);
                return;
            }
            const auto resources = project_resources_;
            submit_work<audioalgo::ProjectResourceReferenceOutcome>(
                [resources, missing_reference, expected_project_path, selected_file] {
                    return resources->relocate(missing_reference, expected_project_path, selected_file);
                },
                [expected_session, expected_document_generation, expected_revision, expected_epoch,
                 expected_project_path, node_id](EditorComponent& target,
                                                 audioalgo::ProjectResourceReferenceOutcome replacement) {
                    if (target.controller_.session_identity() != expected_session ||
                        target.controller_.document_generation() != expected_document_generation ||
                        target.controller_.revision() != expected_revision || target.project_epoch_ != expected_epoch ||
                        target.project_path_ != expected_project_path) {
                        target.set_status("The project changed while preparing the replacement; it was not applied.", true);
                        return;
                    }
                    if (const auto* error = std::get_if<audioalgo::Diagnostic>(&replacement)) {
                        target.set_status(utf8_string(error->code) + ": " + utf8_string(error->message), true);
                        return;
                    }
                    const auto updated = target.controller_.set_parameter(
                        expected_session, node_id, "path", std::get<std::string>(replacement));
                    if (updated.status != audioalgo::editor::ParameterEditStatus::applied) {
                        const auto message = updated.diagnostic
                            ? utf8_string(updated.diagnostic->message) : utf8_literal("The source node was not updated.");
                        target.set_status(message, true);
                        return;
                    }
                    target.mark_changed();
                    target.run_project();
                });
        });
    }

    void toggle_playback() {
        if (preview_service_->is_playing()) {
            preview_service_->pause();
            play_button_.setButtonText("Resume");
            set_status("Paused at " + juce::String(preview_service_->position_seconds(), 2) + " s", false);
            return;
        }
        if (output_file_.empty()) {
            set_status("Run the graph before previewing audio.", true);
            return;
        }
        const auto outcome = preview_service_->play(output_file_);
        if (const auto* error = std::get_if<audioalgo::Diagnostic>(&outcome)) {
            const auto prefix = error->code == "audio_device_unavailable" ? "Audio output unavailable: " : "";
            set_status(prefix + utf8_string(error->message), true);
            play_button_.setButtonText("Play output");
            return;
        }
        play_button_.setButtonText("Pause");
        set_status("Playing rendered output", false);
    }

    void stop_playback_resources() { preview_service_->stop(); }

    std::shared_ptr<const audioalgo::ProjectDocumentService> document_service_;
    std::shared_ptr<const audioalgo::ProjectResourceService> project_resources_;
    std::shared_ptr<audioalgo::AudioPreviewService> preview_service_;
    std::shared_ptr<audioalgo::NodeRegistry> descriptors_owner_;
    audioalgo::NodeRegistry& descriptors_ = *descriptors_owner_;
    audioalgo::editor::EditorController controller_;
    const audioalgo::ProjectModel& project_;
    std::filesystem::path project_path_;
    std::filesystem::path output_file_;
    std::uint64_t project_epoch_ = 1;
    audioalgo::RunReception run_reception_;
    std::shared_ptr<audioalgo::editor::ComponentDispatchState<EditorComponent>> completion_dispatch_state_;
    audioalgo::editor::NodeSelectionState selection_;
    GraphCanvas canvas_;
    NodeInspector inspector_;
    WaveformView waveform_;
    NodeLibrary library_;
    RunStatusPanel status_;
    juce::TextButton new_button_{"New"};
    juce::TextButton open_button_{"Open"};
    juce::TextButton save_button_{"Save"};
    juce::TextButton undo_button_{"Undo"};
    juce::TextButton redo_button_{"Redo"};
    juce::TextButton run_button_{"Run"};
    juce::TextButton play_button_{"Play output"};
    juce::TextButton delete_button_{"Delete selected"};
    audioalgo::editor::EditorWorkerPool worker_pool_{1};
    std::unique_ptr<audioalgo::editor::EditorRunQueue<EditorComponent>> run_queue_;
    audioalgo::AsyncCompletionSlot<std::filesystem::path> file_chooser_completion_;
    std::unique_ptr<juce::FileChooser> active_file_chooser_;
    bool run_after_save_ = false;
    bool shutdown_started_ = false;
};


} // namespace audioalgo::editor
