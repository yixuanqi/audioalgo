#pragma once
#include "EditorNodeSummary.h"
#include "EditorNodeInspector.h"
#include "EditorGeometry.h"
#include "EditorComponentLayout.h"
#include "EditorPanelSplitter.h"
#include "EditorText.h"
#include "EditorExternalProcess.h"
#include "EditorPreferences.h"
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
#include "audio_node_authoring/ProjectCatalog.h"
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
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "EditorUiSupport.h"
#include "EditorGraphCanvas.h"
#include "EditorNodeLibrary.h"
#include "NodeUpgradePanel.h"
#include "NodeAuthoringPanel.h"
#include "EditorWaveformView.h"
#include "EditorRunStatusPanel.h"
#include "EditorRunLogPanel.h"
namespace audioalgo::editor {
struct RunConfiguration { int block_size=512; };
class EditorComponent final : public juce::Component, public juce::DragAndDropContainer {
public:
    EditorComponent(std::shared_ptr<audioalgo::NodeRegistry> descriptors,
                    std::shared_ptr<const audioalgo::ProjectDocumentService> document_service,
                    std::shared_ptr<const audioalgo::ProjectResourceService> project_resources,
                    std::shared_ptr<audioalgo::AudioPreviewService> preview_service,
                    std::shared_ptr<const authoring::DraftStorage> drafts = {}, bool load_demo = true,
                    std::shared_ptr<const authoring::AuthoringChecks> checks = {},
                    std::function<bool()> allow_catalog_prepare = {}, bool user_draft_files = false,
                    std::function<void(bool, const std::filesystem::path&, DraftFileActions::Completion)> draft_picker = {},
                    std::function<authoring::SaveResult(bool, const std::filesystem::path&)> location_persistence = {},
                    std::function<authoring::SaveResult(const std::filesystem::path&)> library_location_persistence = {},
                    RunConfiguration run_configuration = {},
                    std::function<void(const std::string&, const std::filesystem::path&, DraftFileActions::Completion)> directory_picker = {},
                    std::function<void(DraftFileActions::Completion)> editor_picker = {},
                    std::shared_ptr<const EditorPreferencesStore> preferences = {})
        : document_service_(std::move(document_service)), project_resources_(std::move(project_resources)),
          preview_service_(std::move(preview_service)), descriptors_owner_(std::move(descriptors)),
          available_catalog_(std::make_shared<const NodeRegistry>(descriptors_)), controller_(descriptors_), project_(controller_.project()),
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
          library_(descriptors_, [this](std::string id) { add_library_node(id); }, [this] { return project_epoch_; },
                   [this] { new_custom_node(); }, [this] { open_custom_draft(); }, [this] { preview_node_upgrade(); }, [this] { recover_custom_draft(); },
                   [this] { open_node_workspace(); }),
          drafts_(drafts ? std::move(drafts) : std::make_shared<authoring::FileDraftStorage>(
              path_from_juce_file(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                  .getChildFile("AudioAlgo").getChildFile("authoring").getChildFile("drafts")))), checks_(std::move(checks)), allow_catalog_prepare_(std::move(allow_catalog_prepare)), user_draft_files_(user_draft_files), draft_picker_(std::move(draft_picker)), directory_picker_(std::move(directory_picker)), editor_picker_(std::move(editor_picker)), location_persistence_(std::move(location_persistence)), library_location_persistence_(std::move(library_location_persistence)) {
        run_configuration_=run_configuration;
        preferences_store_ = std::move(preferences);
        if (preferences_store_) {
            auto restored = preferences_store_->load();
            if (auto* value = std::get_if<EditorPreferences>(&restored)) {
                code_editor_ = value->editor_executable; last_node_project_ = value->last_project_directory;
            } else preferences_message_ = "Could not load settings: " + std::get<Diagnostic>(restored).message;
        }
        setLookAndFeel(&look_and_feel_);
        library_splitter_.configure("workspace.splitter.library", "Resize node library", true,
            [this](int delta) { panel_sizes_.library += delta; resized(); repaint(); },
            [this] { panel_sizes_.library = 164; resized(); repaint(); });
        inspector_splitter_.configure("workspace.splitter.inspector", "Resize Inspector", true,
            [this](int delta) { panel_sizes_.inspector -= delta; resized(); repaint(); },
            [this] { panel_sizes_.inspector = 270; resized(); repaint(); });
        results_splitter_.configure("workspace.splitter.results", "Resize waveform and logs", false,
            [this](int delta) { panel_sizes_.results -= delta; resized(); },
            [this] { panel_sizes_.results = 168; resized(); });
        for (auto* splitter : {&library_splitter_, &inspector_splitter_, &results_splitter_}) addAndMakeVisible(*splitter);
        completion_dispatch_state_ = std::make_shared<
            audioalgo::editor::ComponentDispatchState<EditorComponent>>(*this);
        inspector_.set_descriptor_registry(descriptors_);
        canvas_.set_connection_context_menu_handler(
            [this](std::string label, std::function<void()> disconnect) {
                show_connection_context_menu(std::move(label), std::move(disconnect));
            });
        canvas_.set_port_connection_context_menu_handler(
            [this](std::vector<GraphCanvas::PortConnectionMenuItem> items) {
                show_port_connection_context_menu(std::move(items));
            });
        canvas_.set_node_connection_context_menu_handler(
            [this](std::vector<std::string> node_ids, std::function<void()> disconnect) {
                const auto label = node_ids.size() > 1
                    ? "Disconnect selected nodes" : "Disconnect all connections";
                show_connection_context_menu(label, std::move(disconnect));
            });

        for (auto* button : {&new_button_, &open_button_, &save_button_, &undo_button_, &redo_button_,
                             &run_button_, &play_button_})
            addAndMakeVisible(*button);
        for (auto* button : {&delete_button_})
            addAndMakeVisible(*button);
        addAndMakeVisible(canvas_);
        addAndMakeVisible(inspector_);
        addAndMakeVisible(waveform_);
        addChildComponent(run_logs_); addAndMakeVisible(waveform_tab_); addAndMakeVisible(logs_tab_);
        waveform_tab_.setComponentID("project.waveform"); logs_tab_.setComponentID("project.logs");
        style_button(waveform_tab_); style_button(logs_tab_);
        waveform_tab_.onClick = [this] { waveform_.setVisible(true); run_logs_.setVisible(false); };
        logs_tab_.onClick = [this] { waveform_.setVisible(false); run_logs_.setVisible(true); };
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
        new_button_.setComponentID("project.new");
        open_button_.onClick = [this] { open_project(); };
        save_button_.onClick = [this] { save_project(); };
        undo_button_.onClick = [this] { undo_topology(); };
        redo_button_.onClick = [this] { redo_topology(); };
        run_button_.onClick = [this] { run_project(); };
        run_button_.setComponentID("project.run"); save_button_.setComponentID("project.save");
        canvas_.setComponentID("project.canvas"); status_.setComponentID("project.status");
        play_button_.setComponentID("project.play");
        play_button_.onClick = [this] { toggle_playback(); };
        play_button_.setEnabled(false);
        run_logs_.enable_history([this]{refresh_run_log_history();},[this](std::optional<RunLogArchiveReference> selected){select_run_log_history(std::move(selected));});
        delete_button_.onClick = [this] { canvas_.delete_selected(); };

        auto initial_project = audioalgo::ProjectModel{};
        initial_project.project_id = new_id();
        initial_project.name = "Untitled Audio Graph";
        controller_.adopt_project(std::move(initial_project));
        if (load_demo) load_staged_demo();
        setSize(audioalgo::editor::default_editor_window_size.width,
                audioalgo::editor::default_editor_window_size.height);
        update_history_controls();
    }

    void dragOperationEnded(const juce::DragAndDropTarget::SourceDetails& details) override {
        auto* button = dynamic_cast<NodeLibraryButton*>(details.sourceComponent.get());
        if (button != nullptr)
            canvas_.finish_library_drag(button->gesture_project_epoch());
    }

    // Opens a project from an explicit path; shares the File→Open completion flow (command-line launch).
    void open_project_file(const std::filesystem::path& path) {
        if (path.empty())
            return;
        // Document loading shares the bounded worker with rendering. Cancel on
        // admission so an active render cannot prevent the Open that replaces it.
        // Session/catalog adoption still occurs only after a successful load.
        if (run_queue_) run_queue_->cancel();
        const auto expected_session = controller_.session_identity();
        const auto expected_document_generation = controller_.document_generation();
        const auto service = document_service_;
        const auto checks = checks_;
        const auto base = available_catalog_;
        struct OpenResult { ProjectDocumentLoadOutcome document; authoring::CatalogResolution resolution; };
        submit_work<OpenResult>(
            [service, path, checks, base] {
                OpenResult result{service->load(path), {base, {}}};
                if (const auto* project = std::get_if<ProjectModel>(&result.document)) {
                    const auto selections = authoring::managed_selections(*project);
                    if (checks) result.resolution = checks->resolve_catalog(base, selections, path.parent_path());
                    else for (const auto& selection : selections)
                        result.resolution.diagnostics.push_back({"managed_version_unavailable", "No exact package resolver is configured", selection.node_id});
                }
                return result;
            },
            [path, expected_session, expected_document_generation]
            (EditorComponent& target, OpenResult outcome) {
                auto catalog = std::make_unique<NodeRegistry>(*outcome.resolution.catalog);
                const auto opened = audioalgo::EditorProjectDocuments::complete_open(
                    target.controller_, std::move(outcome.document), expected_session, expected_document_generation,
                    [&target, &catalog] { target.advance_project_epoch(); target.descriptors_.swap(*catalog); });
                if (const auto* error = std::get_if<audioalgo::Diagnostic>(&opened)) {
                    if (error->code != "project_session_stale")
                        target.set_status(utf8_string(error->code) + ": " + utf8_string(error->message), true);
                    return;
                }
                target.update_history_controls();
                target.project_path_ = path;
                if (!outcome.resolution.diagnostics.empty()) {
                    const auto& error = outcome.resolution.diagnostics.front();
                    target.set_status(utf8_string(error.code) + ": " + utf8_string(error.message), true);
                    return;
                }
                target.set_status("Opened " + utf8_string(path_to_utf8(path.filename())), false);
            });
    }

    ~EditorComponent() override {
        // The panel may release a prepared catalog through its host callback.
        // Do so while all host members are still alive.
        authoring_panel_.reset();
        file_chooser_completion_.close();
        completion_dispatch_state_->invalidate();
        if (!shutdown_started_) {
            advance_project_epoch();
            active_file_chooser_.reset();
            preview_service_->shutdown();
            jassert(worker_pool_.get_num_jobs() == 0);
        }
        worker_pool_.join_shutdown();
        setLookAndFeel(nullptr);
    }

    void begin_shutdown(std::function<void()> on_drained) {
        if (shutdown_started_)
            return;

        if (authoring_panel_) {
            after_draft_close_ = std::move(on_drained);
            authoring_panel_->request_close([this] { after_draft_close_ = {}; });
            return;
        }

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
            }, [store = preferences_store_] {
                if (store)
                    if (const auto error = store->flush())
                        juce::Logger::writeToLog("Editor preferences were not saved before closing: " + utf8_string(error->message));
            });
        jassert(started);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(background);
        g.setColour(panel);
        g.fillRect(0, 0, getWidth(), 54);
        g.fillRect(0, 54, panel_sizes_.library, getHeight() - 54);
        g.setColour(signal);
        g.setFont(juce::FontOptions(19.0f, juce::Font::bold));
        g.drawText("AudioAlgo", 18, 10, 132, 28, juce::Justification::centredLeft);
        g.setColour(muted);
        g.setFont(juce::FontOptions(10.5f));
        g.drawText("OFFLINE WORKBENCH", 18, 33, 132, 14, juce::Justification::centredLeft);
    }

    void resized() override {
        const auto layout = audioalgo::editor::make_editor_component_layout(getWidth(), getHeight(), panel_sizes_);
        panel_sizes_ = {layout.library_pane.width, layout.inspector.width, layout.waveform.height};
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
        set_bounds(library_, layout.library_pane);
        set_bounds(library_splitter_, layout.library_splitter);
        set_bounds(inspector_splitter_, layout.inspector_splitter);
        set_bounds(results_splitter_, layout.results_splitter);
        set_bounds(delete_button_, layout.delete_button);
        set_bounds(canvas_, layout.canvas);
        set_bounds(inspector_, layout.inspector);
        set_bounds(waveform_, layout.waveform);
        auto results = waveform_.getBounds();
        waveform_tab_.setBounds(results.removeFromTop(26).withWidth(90));
        logs_tab_.setBounds(waveform_tab_.getRight() + 4, waveform_tab_.getY(), 100, 26);
        waveform_.setBounds(results); run_logs_.setBounds(results);
        set_bounds(status_, layout.status);
        if (authoring_panel_) authoring_panel_->setBounds(getLocalBounds().reduced(18));
        if (upgrade_panel_) upgrade_panel_->setBounds(getLocalBounds().reduced(36));
    }

    const audioalgo::editor::EditorController& controller() const { return controller_; }
    [[nodiscard]] EditorRunQueue<EditorComponent>::Mailbox::Counts background_run_counts() const {
        return run_queue_ ? run_queue_->counts() : EditorRunQueue<EditorComponent>::Mailbox::Counts{};
    }
    [[nodiscard]] int background_job_count() const noexcept { return worker_pool_.get_num_jobs(); }

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
                controller_.mark_result_current();
                output_file_ = result.output_file;
                waveform_.set_waveform(std::move(waveform));
                set_status("Completed run " + utf8_string(result.run_id) + utf8_literal(" · ") +
                           juce::String(static_cast<juce::int64>(result.frames)) + utf8_literal(" frames · ") +
                           utf8_string(result.output_sha256.substr(0, 12)), false);
                play_button_.setButtonText("Play output");
                play_button_.setEnabled(true);
            });
        juce::ignoreUnused(completion_applied);
    }
    void run_log_received(NodeLogEvent event) {
        const auto applied = apply_if_current_run(run_reception_, event.identity, project_.project_id, project_epoch_, project_.revision, [&] {
            if(viewing_archived_logs_)return;
            run_logs_.append(event); waveform_.setVisible(false); run_logs_.setVisible(true);
        });
        juce::ignoreUnused(applied);
    }
    void run_logs_received(std::deque<NodeLogEvent> events) {
        std::deque<NodeLogEvent> current;
        for (auto& event : events) {
            const auto applied = apply_if_current_run(run_reception_, event.identity, project_.project_id, project_epoch_, project_.revision, [&] {
                if (!viewing_archived_logs_) current.push_back(std::move(event));
            });
            juce::ignoreUnused(applied);
        }
        if (!current.empty()) {
            run_logs_.append_batch(current); waveform_.setVisible(false); run_logs_.setVisible(true);
        }
    }

private:
    friend struct EditorComponentTestAccess;

    void cancel_log_history_read() {
        if(log_history_cancel_)log_history_cancel_->store(true);
        ++log_history_generation_;viewing_archived_logs_=false;
    }
    void refresh_run_log_history() {
        if(project_path_.empty()){run_logs_.set_history_status("Save the project before browsing logs");return;}
        cancel_log_history_read();log_history_cancel_=std::make_shared<std::atomic<bool>>(false);
        const auto token=log_history_cancel_;const auto generation=log_history_generation_,epoch=project_epoch_;
        const auto directory=project_path_.parent_path()/"run-logs";const auto project=project_.project_id;const auto active=run_reception_.active();
        run_logs_.set_history_busy(true);waveform_.setVisible(false);run_logs_.setVisible(true);
        struct HistoryResult{RunLogArchiveList list;RunLogArchiveTail current;};
        const auto submitted=submit_work<HistoryResult>([directory,project,active,token]{
            const auto cancelled=[token]{return token->load();};HistoryResult result{RunLogArchiveReader::list(directory,project,cancelled),{}};
            if(active)for(const auto& archive:result.list.runs)if(archive.identity==*active){result.current=RunLogArchiveReader::load(archive,cancelled);break;}
            return result;
        },[generation,epoch](EditorComponent& target,HistoryResult result){
            if(generation!=target.log_history_generation_||epoch!=target.project_epoch_)return;
            auto notice=result.list.notice;target.run_logs_.set_history_list(std::move(result.list));target.run_logs_.clear_entries();
            for(const auto& event:result.current.events)target.run_logs_.append(event);
            target.run_logs_.set_history_busy(false);
            if(!result.current.notice.empty())notice+=(notice.empty()?"":"; ")+result.current.notice;
            target.run_logs_.set_history_status(notice.empty()?"Current Run":notice);
        });
        if(!submitted)run_logs_.set_history_busy(false);
    }
    void select_run_log_history(std::optional<RunLogArchiveReference> selected) {
        cancel_log_history_read();viewing_archived_logs_=selected.has_value();log_history_cancel_=std::make_shared<std::atomic<bool>>(false);
        const auto token=log_history_cancel_;const auto generation=log_history_generation_,epoch=project_epoch_;
        const auto directory=project_path_.parent_path()/"run-logs";const auto project=project_.project_id;const auto active=run_reception_.active();
        run_logs_.clear_entries();run_logs_.set_history_busy(true);waveform_.setVisible(false);run_logs_.setVisible(true);
        const auto submitted=submit_work<RunLogArchiveTail>([directory,project,active,selected,token]{
            const auto cancelled=[token]{return token->load();};
            if(selected)return RunLogArchiveReader::load(*selected,cancelled);
            if(active){const auto list=RunLogArchiveReader::list(directory,project,cancelled);for(const auto& archive:list.runs)if(archive.identity==*active)return RunLogArchiveReader::load(archive,cancelled);}
            return RunLogArchiveTail{};
        },[generation,epoch,selected](EditorComponent& target,RunLogArchiveTail result){
            if(generation!=target.log_history_generation_||epoch!=target.project_epoch_)return;
            for(const auto& event:result.events)target.run_logs_.append(event);
            target.run_logs_.set_history_busy(false);
            auto label=selected?"Archived Run "+selected->identity.request_id:std::string("Current Run");
            if(!result.notice.empty())label+=" · "+result.notice;
            target.run_logs_.set_history_status(label);
        });
        if(!submitted)run_logs_.set_history_busy(false);
    }
    void preview_node_upgrade() {
        if (authoring_panel_) { authoring_panel_->toFront(true); return; }
        std::string package_id;
        for (const auto& node : project_.nodes) if (node.managed_version) {
            const auto* latest = available_catalog_->find(node.type_id);
            if (latest && latest->plugin_origin && latest->plugin_origin->plugin_id == node.managed_version->package_id &&
                latest->plugin_origin->managed_content_sha256 != node.managed_version->content_sha256) {
                package_id = node.managed_version->package_id; break;
            }
        }
        if (package_id.empty()) { set_status("Project nodes already use the available versions.", false); return; }
        const auto family = [&package_id](const NodeDescriptor& descriptor) {
            return descriptor.plugin_origin && !descriptor.plugin_origin->managed_content_sha256.empty() && descriptor.plugin_origin->plugin_id == package_id;
        };
        auto candidate = std::make_shared<NodeRegistry>(descriptors_.subset([&](const NodeDescriptor& descriptor) { return !family(descriptor); }));
        if (!candidate->merge_disjoint(available_catalog_->subset(family))) { set_status("Upgrade type conflict; project preserved.", true); return; }
        const auto preview = controller_.preview_managed_upgrade(*candidate, package_id);
        upgrade_panel_ = std::make_unique<NodeUpgradePanel>(preview, [this, preview, candidate] {
            const auto result = controller_.commit_managed_upgrade(preview, *candidate);
            if (result.status == ProjectEditStatus::applied) {
                cancel_catalog_resolution();
                descriptors_.swap(*candidate);
                inspector_.show_node(canvas_.selected_node_id());
                mark_changed();
                set_status("Package instances upgraded. Previous results are out of date.", false);
            } else if (!result.diagnostics.empty()) {
                const auto& error = result.diagnostics.front(); set_status(utf8_string(error.code) + ": " + utf8_string(error.message), true);
            }
            upgrade_panel_.reset();
        }, [this] { upgrade_panel_.reset(); });
        addAndMakeVisible(*upgrade_panel_); upgrade_panel_->toFront(true); resized();
    }
    void choose_draft_file(bool save, const std::filesystem::path& initial, DraftFileActions::Completion completion) {
        if (draft_picker_) { draft_picker_(save, initial, std::move(completion)); return; }
        const auto location = initial.empty() ? juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Custom node.aadraft") : to_juce_file(initial);
        launch_file_chooser(save ? "Save custom node draft" : "Open custom node draft", location, "*.aadraft",
            (save ? juce::FileBrowserComponent::saveMode : juce::FileBrowserComponent::openMode) | juce::FileBrowserComponent::canSelectFiles,
            [save, completion = std::move(completion)](const std::filesystem::path& path) {
                auto selected = path;
                if (save && !selected.empty() && selected.extension().empty()) selected += ".aadraft";
                completion(std::move(selected));
            }, true);
    }
    void choose_directory(const juce::String& title, const std::filesystem::path& initial,
                          DraftFileActions::Completion completion) {
        if (directory_picker_) { directory_picker_(title.toStdString(), initial, std::move(completion)); return; }
        launch_file_chooser(title, initial.empty() ? juce::File::getSpecialLocation(juce::File::userDocumentsDirectory) : to_juce_file(initial), {},
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            std::move(completion), true);
    }
    std::filesystem::path code_editor_;
    std::filesystem::path last_node_project_;
    std::shared_ptr<const EditorPreferencesStore> preferences_store_;
    std::string preferences_message_;
    std::uint64_t preferences_request_ = 0;
    void remember_editor_preferences() {
        if (!preferences_store_) return;
        preferences_store_->stage({code_editor_,last_node_project_});
        const auto request = ++preferences_request_;
        const auto store = preferences_store_;
        preferences_message_ = "Saving editor settings...";
        if (!submit_work<authoring::SaveResult>([store] { return store->flush(); },
            [request](EditorComponent& target, authoring::SaveResult result) {
                if (request != target.preferences_request_) return;
                target.preferences_message_ = result ? "Session only; settings not saved: " + result->message : "";
                if (target.authoring_panel_) target.authoring_panel_->refresh_preferences();
            })) preferences_message_ = "Session only; settings not saved because the editor is closing";
        if (authoring_panel_) authoring_panel_->refresh_preferences();
    }
    void remember_node_project(const std::filesystem::path& directory) {
        last_node_project_ = directory; remember_editor_preferences();
    }
    void open_node_workspace() {
        if (authoring_panel_) { authoring_panel_->toFront(true); return; }
        choose_directory("Open node project folder", last_node_project_, [this](std::filesystem::path directory) {
            if (directory.empty()) return;
            const auto epoch = project_epoch_;
            submit_work<authoring::WorkspaceOutcome>(
                [directory] { return authoring::WorkspaceStore{}.open(directory); },
                [epoch](EditorComponent& target, authoring::WorkspaceOutcome outcome) {
                    if (target.authoring_panel_ || target.project_epoch_ != epoch) return;
                    if (auto* error = std::get_if<Diagnostic>(&outcome)) {
                        target.set_status(utf8_string(error->code) + ": " + utf8_string(error->message), true);
                        return;
                    }
                    auto workspace = std::get<authoring::Workspace>(std::move(outcome));
                    target.remember_node_project(workspace.directory);
                    auto draft = authoring::workspace_identity(workspace);
                    std::uint64_t bytes = 0;
                    for (const auto& file : workspace.files) bytes += file.bytes;
                    DraftFileActions files;
                    files.workspace = DraftFileActions::WorkspaceBinding{workspace.directory, workspace.content_sha256,
                                                                        workspace.files.size(), bytes, workspace};
                    files.initial_location_message = "Node project: " + path_to_utf8(workspace.directory) + " | " +
                                                     std::to_string(workspace.files.size()) + " files";
                    target.show_draft(std::move(draft), true, std::move(files));
                });
        });
    }
    void show_draft(authoring::Draft draft, bool saved, DraftFileActions files = {}) {
        if (user_draft_files_ && !files.workspace) {
            files.choose_save = [this](const std::filesystem::path& initial, DraftFileActions::Completion completion) { choose_draft_file(true, initial, std::move(completion)); };
            files.confirm_overwrite = [this](const std::filesystem::path& path, std::function<void(bool)> done) {
                juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon, "Replace draft file?", "Replace the existing draft at:\n" + utf8_string(path_to_utf8(path)) + "\nExternal changes after this confirmation will still be rejected.", "Replace", "Cancel", this,
                    juce::ModalCallbackFunction::create([done = std::move(done)](int result) { done(result == 1); }));
            };
            files.choose_directory = [this](bool recovery, DraftFileActions::Completion completion) {
                const auto current = recovery ? drafts_->directory() : checks_ ? checks_->work_directory() : std::filesystem::path{};
                launch_file_chooser(recovery ? "Choose recovery folder" : "Choose check / build work folder", current.empty() ? juce::File::getSpecialLocation(juce::File::userDocumentsDirectory) : to_juce_file(current), {},
                    juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, std::move(completion), true);
            };
            files.persist_directory = location_persistence_;
            files.directory_override = [](bool recovery) { return juce::SystemStats::getEnvironmentVariable(recovery ? "AUDIOALGO_AUTHORING_RECOVERY_ROOT" : "AUDIOALGO_AUTHORING_WORK_ROOT", {}).toStdString(); };
            files.locations_changed = [this](auto checks, auto storage) { checks_ = std::move(checks); drafts_ = std::move(storage); };
            files.choose_library_directory = [this](DraftFileActions::Completion completion) {
                launch_file_chooser("Copy node library to an empty folder (next startup)", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), {},
                    juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, std::move(completion), true);
            };
            files.persist_library_directory = library_location_persistence_;
            files.library_change_allowed = [this] { return !library_restart_pending_ && worker_pool_.get_num_jobs() == 0; };
            files.allow_apply = [this] { return !library_restart_pending_; };
            files.library_change_queued = [this] { library_restart_pending_ = true; };
            if (library_restart_pending_) files.initial_location_message += (files.initial_location_message.empty() ? "" : " | ") + std::string("Library change queued; save and restart before applying new versions");
            files.choose_workspace_directory = [this](DraftFileActions::Completion completion) {
                choose_directory("Create node project folder", last_node_project_.parent_path(), std::move(completion));
            };
            files.choose_workspace_for_edit = [this](DraftFileActions::Completion completion) {
                choose_directory("Open an existing node project or choose an empty folder to create one", last_node_project_, std::move(completion));
            };
            if (!files.create_workspace)
                files.create_workspace = [](const std::filesystem::path& target, const authoring::Draft& source) {
                    return authoring::WorkspaceStore{}.create(target, source);
                };
        }
        files.editor_description = [this] { return path_to_utf8(code_editor_); };
        files.last_project_directory = [this] { return last_node_project_; };
        files.preferences_status = [this] { return preferences_message_; };
        files.workspace_opened = [this](const std::filesystem::path& directory) { remember_node_project(directory); };
        files.choose_editor = [this](std::function<void(bool)> done) {
            juce::Component::SafePointer<EditorComponent> safe(this);
            auto selected = [safe,done=std::move(done)](std::filesystem::path executable) {
                if (!safe) return;
                const bool chosen = !executable.empty();
                if (chosen) { safe->code_editor_ = std::move(executable); safe->remember_editor_preferences(); }
                done(chosen);
            };
            if (editor_picker_) { editor_picker_(std::move(selected)); return; }
            if (active_file_chooser_) { selected({}); return; }
            launch_file_chooser("Choose code editor executable", code_editor_.empty() ? juce::File::getSpecialLocation(juce::File::userHomeDirectory) : to_juce_file(code_editor_), "*.exe",
                juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                std::move(selected), true);
        };
        files.open_project = [this](const std::filesystem::path& directory, const std::string& file, int line, int column) -> std::optional<std::string> {
            if (code_editor_.empty()) {
                if (to_juce_file(directory).startAsProcess()) return {};
                return "Could not open the node project folder; choose an editor executable";
            }
            if (!to_juce_file(code_editor_).getFileExtension().equalsIgnoreCase(".exe")) return "Choose an editor .exe; shell scripts are not executable editor configurations";
            juce::StringArray arguments{to_juce_file(code_editor_).getFullPathName()};
            if (!file.empty()) {
                const auto path = to_juce_file(directory / std::filesystem::u8path(file)).getFullPathName();
                if (to_juce_file(code_editor_).getFileName().equalsIgnoreCase("Code.exe")) { arguments.add("--goto"); arguments.add(path + ":" + juce::String(std::max(1, line)) + ":" + juce::String(std::max(1, column))); }
                else arguments.add(path);
            } else arguments.add(to_juce_file(directory).getFullPathName());
            return launch_external_editor(arguments);
        };
        authoring_panel_ = std::make_unique<NodeAuthoringPanel>(std::move(draft), saved, drafts_, [this] {
            auto after_close = std::move(after_draft_close_);
            authoring_panel_.reset();
            if (after_close) begin_shutdown(std::move(after_close));
        }, checks_, [this] { return available_catalog_; },
        [this](authoring::AppliedPackage applied) {
            cancel_catalog_resolution();
            available_catalog_ = std::move(applied.catalog);
            const auto selections = authoring::managed_selections(project_);
            const bool resolve_current = !selections.empty() && selections != prepared_project_locks_;
            if (selections.empty()) descriptors_.swap(*prepared_default_catalog_);
            else if (!resolve_current) descriptors_.swap(*prepared_catalog_);
            library_.publish_refresh(std::move(*prepared_library_));
            prepared_library_.reset(); prepared_catalog_.reset(); prepared_default_catalog_.reset();
            prepared_project_locks_.clear();
            canvas_.cancel_library_drag();
            ++catalog_generation_;
            set_status("Custom node available in node library", false);
            if (resolve_current) refresh_project_catalog();
        }, [this](const authoring::AppliedPackage& applied) -> std::optional<authoring::SourceDiagnostic> {
            if (allow_catalog_prepare_ && !allow_catalog_prepare_())
                return authoring::SourceDiagnostic{"authoring_catalog_refresh_failed", "Node library preparation failed"};
            auto catalog = std::make_unique<NodeRegistry>(*applied.catalog);
            std::set<std::string> locked_packages;
            for (const auto& node : project_.nodes)
                if (node.managed_version) locked_packages.insert(node.managed_version->package_id);
            const auto locked = [&locked_packages](const NodeDescriptor& descriptor) {
                return descriptor.plugin_origin && !descriptor.plugin_origin->managed_content_sha256.empty() &&
                    locked_packages.contains(descriptor.plugin_origin->plugin_id);
            };
            *catalog = catalog->subset([&locked](const NodeDescriptor& descriptor) { return !locked(descriptor); });
            if (!catalog->merge_disjoint(descriptors_.subset(locked)))
                return authoring::SourceDiagnostic{"managed_version_conflict", "Project package types conflict with the available catalog"};
            auto library = library_.prepare_refresh(*applied.catalog);
            auto defaults = std::make_unique<NodeRegistry>(*applied.catalog);
            auto selections = authoring::managed_selections(project_);
            prepared_catalog_ = std::move(catalog); prepared_library_ = std::move(library);
            prepared_default_catalog_ = std::move(defaults); prepared_project_locks_ = std::move(selections);
            return {};
        }, [this] {
            prepared_library_.reset(); prepared_catalog_.reset(); prepared_default_catalog_.reset();
            prepared_project_locks_.clear();
        }, std::move(files));
        addAndMakeVisible(*authoring_panel_); authoring_panel_->toFront(true); resized();
    }
    void refresh_project_catalog() {
        if (!checks_) return;
        const auto request = ++catalog_resolution_generation_;
        catalog_resolution_pending_ = true;
        run_button_.setEnabled(false);
        const auto expected_session = controller_.session_identity();
        const auto expected_generation = controller_.document_generation();
        const auto expected_catalog = catalog_generation_;
        submit_work<authoring::CatalogResolution>(
            [service = checks_, base = available_catalog_, selections = authoring::managed_selections(project_), directory = project_path_.parent_path()] {
                return service->resolve_catalog(base, selections, directory);
            }, [request, expected_session, expected_generation, expected_catalog](EditorComponent& target, authoring::CatalogResolution result) {
                if (request != target.catalog_resolution_generation_) return;
                if (target.catalog_generation_ != expected_catalog || target.controller_.session_identity() != expected_session || target.controller_.document_generation() != expected_generation) {
                    target.refresh_project_catalog();
                    return;
                }
                auto catalog = *result.catalog;
                target.descriptors_.swap(catalog);
                target.cancel_catalog_resolution();
                target.inspector_.show_node(target.canvas_.selected_node_id());
                target.canvas_.repaint();
                if (!result.diagnostics.empty()) {
                    const auto& error = result.diagnostics.front();
                    target.set_status(utf8_string(error.code) + ": " + utf8_string(error.message), true);
                }
            });
    }
    void cancel_catalog_resolution() {
        ++catalog_resolution_generation_;
        catalog_resolution_pending_ = false;
        run_button_.setEnabled(true);
    }
    void new_custom_node() {
        if (authoring_panel_) { authoring_panel_->toFront(true); return; }
        auto draft = authoring::python_draft(new_id());
        auto definition = juce::JSON::parse(utf8_string(draft.declaration));
        definition.getDynamicObject()->setProperty("type_id", "user.custom." + utf8_string(draft.id));
        draft.declaration = juce::JSON::toString(definition, false).toStdString();
        show_draft(std::move(draft), false);
    }
    void load_draft(const std::string& id) {
        if (user_draft_files_) {
            submit_work<authoring::RecoveryOutcome>([storage = drafts_, id] { return storage->load_recovery(id); },
                [](EditorComponent& target, authoring::RecoveryOutcome outcome) {
                    if (target.authoring_panel_) return;
                    if (auto* error = std::get_if<Diagnostic>(&outcome)) { target.set_status(utf8_string(error->message), true); return; }
                    auto recovered = std::get<authoring::DraftRecovery>(std::move(outcome));
                    DraftFileActions files; files.path = std::move(recovered.original_file); files.expected_bytes = std::move(recovered.expected_bytes);
                    files.initial_location_message = "Recovered copy: " + path_to_utf8(recovered.recovery_file);
                    if (recovered.updated_unix_seconds > 0) files.initial_location_message += " | Updated: " + juce::Time(recovered.updated_unix_seconds * 1000).formatted("%Y-%m-%d %H:%M:%S").toStdString();
                    if (!recovered.warning.empty()) files.initial_location_message += " | " + recovered.warning;
                    if (!recovered.warning.empty()) target.set_status("Draft text recovered; original location unavailable: " + utf8_string(recovered.warning), true);
                    target.show_draft(std::move(recovered.document), false, std::move(files));
                });
            return;
        }
        submit_work<authoring::LoadResult>([storage = drafts_, id] { return storage->load(id); },
            [](EditorComponent& target, authoring::LoadResult result) {
                if (target.authoring_panel_) return;
                if (auto* draft = std::get_if<authoring::Draft>(&result)) target.show_draft(std::move(*draft), !target.user_draft_files_);
                else target.set_status(utf8_string(std::get<Diagnostic>(result).message), true);
            });
    }
    void open_custom_draft() {
        if (authoring_panel_) { authoring_panel_->toFront(true); return; }
        if (user_draft_files_) {
            auto safe = juce::Component::SafePointer<EditorComponent>(this);
            choose_draft_file(false, {}, [safe](std::filesystem::path path) {
                if (!safe || path.empty() || safe->authoring_panel_) return;
                safe->submit_work<authoring::DraftFileOutcome>([path] { return authoring::DraftFiles{}.load(path); },
                    [path](EditorComponent& target, authoring::DraftFileOutcome outcome) {
                        if (target.authoring_panel_) return;
                        if (auto* error = std::get_if<Diagnostic>(&outcome)) { target.set_status(utf8_string(error->message), true); return; }
                        auto file = std::get<authoring::DraftFile>(std::move(outcome));
                        DraftFileActions files; files.path = path; files.expected_bytes = std::move(file.bytes);
                        target.show_draft(std::move(file.document), true, std::move(files));
                    });
            });
            return;
        }
        recover_custom_draft();
    }
    void recover_custom_draft() {
        if (authoring_panel_) { authoring_panel_->toFront(true); return; }
        submit_work<authoring::ListResult>([storage = drafts_] { return storage->list(); },
            [](EditorComponent& target, authoring::ListResult result) {
                if (auto* error = std::get_if<Diagnostic>(&result)) { target.set_status(utf8_string(error->message), true); return; }
                auto ids = std::get<std::vector<std::string>>(std::move(result));
                if (ids.empty()) { target.set_status("No saved custom node drafts.", false); return; }
                if (ids.size() == 1) { target.load_draft(ids.front()); return; }
                juce::PopupMenu menu;
                for (std::size_t i = 0; i < ids.size(); ++i) menu.addItem(static_cast<int>(i + 1), utf8_string(ids[i]));
                juce::Component::SafePointer<EditorComponent> safe(&target);
                menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target.library_),
                    [safe, ids = std::move(ids)](int choice) {
                        if (safe != nullptr && choice > 0 && static_cast<std::size_t>(choice) <= ids.size()) safe->load_draft(ids[choice - 1]);
                    });
            });
    }
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
        if (controller_.results_stale()) {
            run_reception_.invalidate();
            clear_result_presentation();
        }
        update_history_controls();
        canvas_.repaint();
        const auto star = project_path_.empty() ? utf8_literal(" · unsaved") :
            (controller_.dirty() ? utf8_literal(" · modified") : juce::String{});
        set_status(utf8_string(project_.name) + star, false);
    }

    void advance_project_epoch() {
        (void)juce::PopupMenu::dismissAllActiveMenus();
        cancel_log_history_read();
        cancel_catalog_resolution();
        upgrade_panel_.reset();
        run_reception_.invalidate();
        run_logs_.clear();
        if (run_queue_) run_queue_->cancel();
        ++project_epoch_;
        if (project_epoch_ == 0)
            ++project_epoch_;
        if (!file_chooser_for_draft_) { active_file_chooser_.reset(); file_chooser_completion_.invalidate(); }
        run_after_save_ = false;
        canvas_.set_project_epoch(project_epoch_);
        canvas_.select({});
        clear_result_presentation();
    }

    void update_history_controls() {
        undo_button_.setEnabled(controller_.history().can_undo());
        redo_button_.setEnabled(controller_.history().can_redo());
    }

    void undo_topology() {
        const auto versions = authoring::managed_selections(project_);
        if (!controller_.undo())
            return;
        if (versions != authoring::managed_selections(project_)) refresh_project_catalog();
        refresh_history_selection();
        mark_changed();
        set_status("Change undone.", false);
    }

    void redo_topology() {
        const auto versions = authoring::managed_selections(project_);
        if (!controller_.redo())
            return;
        if (versions != authoring::managed_selections(project_)) refresh_project_catalog();
        refresh_history_selection();
        mark_changed();
        set_status("Change redone.", false);
    }

    void refresh_history_selection() {
        const auto node_id = canvas_.selected_node_id();
        canvas_.select(controller_.node_snapshot(node_id) ? node_id : std::string{});
    }

    void new_project() {
        auto catalog = *available_catalog_;
        advance_project_epoch();
        descriptors_.swap(catalog);
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
        const auto expected_catalog = catalog_generation_;
        juce::Component::SafePointer<EditorComponent> safe_this(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&canvas_),
            [safe_this, type_id, node_id, target, position, source_epoch, expected_revision, expected_catalog,
             choices = std::move(choices)](int selected_id) mutable {
                if (safe_this == nullptr || safe_this->catalog_generation_ != expected_catalog || selected_id <= 0 ||
                    static_cast<std::size_t>(selected_id) > choices.size())
                    return;
                safe_this->commit_library_insertion(
                    type_id, node_id, target, position, source_epoch, expected_revision,
                    choices[static_cast<std::size_t>(selected_id - 1)]);
            });
    }

    void show_connection_context_menu(std::string label, std::function<void()> disconnect) {
        juce::PopupMenu menu;
        menu.addItem(1, utf8_string(label));
        juce::Component::SafePointer<EditorComponent> safe_this(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&canvas_),
            [safe_this, disconnect = std::move(disconnect)](int selected_id) mutable {
                if (safe_this != nullptr && selected_id == 1 && disconnect)
                    disconnect();
            });
    }

    void show_port_connection_context_menu(std::vector<GraphCanvas::PortConnectionMenuItem> items) {
        if (items.empty())
            return;
        juce::PopupMenu menu;
        std::vector<std::function<void()>> actions;
        actions.reserve(items.size());
        for (std::size_t index = 0; index < items.size(); ++index) {
            menu.addItem(static_cast<int>(index + 1), utf8_string(items[index].label));
            actions.push_back(std::move(items[index].action));
        }
        juce::Component::SafePointer<EditorComponent> safe_this(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&canvas_),
            [safe_this, actions = std::move(actions)](int selected_id) mutable {
                if (safe_this == nullptr || selected_id <= 0 ||
                    static_cast<std::size_t>(selected_id) > actions.size())
                    return;
                auto& action = actions[static_cast<std::size_t>(selected_id - 1)];
                if (action)
                    action();
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
        const auto checks = checks_;
        const auto source_directory = project_path_.parent_path();
        return submit_work<audioalgo::ProjectDocumentSaveOutcome>(
            [service, checks, source_directory, path, snapshot = std::move(snapshot)]() -> ProjectDocumentSaveOutcome {
                const auto selections = authoring::managed_selections(snapshot);
                if (!selections.empty()) {
                    if (!checks) return Diagnostic{"authoring_carry_failed", "No exact package service is configured"};
                    if (auto error = checks->carry_packages(selections, source_directory, path.parent_path())) return Diagnostic{error->code, error->message};
                }
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
                             std::function<void(const std::filesystem::path&)> completion, bool survives_project_switch = false) {
        if (active_file_chooser_ != nullptr) {
            set_status("Finish the open file selection first.", true);
            return;
        }
        const auto completion_token = file_chooser_completion_.begin(std::move(completion));
        if (completion_token == 0)
            return;
        file_chooser_for_draft_ = survives_project_switch;
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
        file_chooser_for_draft_ = false;
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
        launch_file_chooser("Open AudioAlgo project", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
                            "*.aaproj", juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                            [this](const std::filesystem::path& path) {
            open_project_file(path);
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
        if (catalog_resolution_pending_) { set_status("Resolving exact node versions before Run…", false); return; }
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
        clear_result_presentation();
        const auto revision = project_.revision;
        const audioalgo::RunIdentity identity{project_.project_id, project_epoch_, new_id(), revision};
        run_reception_.begin(identity);
        cancel_log_history_read();
        run_logs_.clear();
        const auto run_root = project_path_.parent_path() / "runs";
        const auto output_directory = run_root / ("run-" + new_id());
        audioalgo::RunRequest request{project_, project_path_, output_directory, run_configuration_.block_size};
        request.identity = identity;
        set_status("Rendering revision " + juce::String(static_cast<juce::int64>(revision)) + utf8_literal(" in the background…"), false);
        if (!run_queue_) run_queue_ = std::make_unique<audioalgo::editor::EditorRunQueue<EditorComponent>>(
            worker_pool_, completion_dispatch_state_, descriptors_owner_, project_resources_,
            [](EditorComponent& target, audioalgo::RunOutcome outcome,
               WaveformChannels waveform, audioalgo::RunIdentity completed_identity) {
                target.run_finished(std::move(outcome), std::move(waveform), std::move(completed_identity));
            }, EditorRunQueue<EditorComponent>::LogCompletion{},
            [](EditorComponent& target, std::deque<NodeLogEvent> events) { target.run_logs_received(std::move(events)); });
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

    void clear_result_presentation() {
        stop_playback_resources();
        output_file_.clear();
        waveform_.set_waveform({});
        play_button_.setButtonText("Play output");
        play_button_.setEnabled(false);
    }

    EditorLookAndFeel look_and_feel_;
    EditorPanelSizes panel_sizes_;
    PanelSplitter library_splitter_, inspector_splitter_, results_splitter_;
    std::shared_ptr<const audioalgo::ProjectDocumentService> document_service_;
    std::shared_ptr<const audioalgo::ProjectResourceService> project_resources_;
    std::shared_ptr<audioalgo::AudioPreviewService> preview_service_;
    std::shared_ptr<audioalgo::NodeRegistry> descriptors_owner_;
    audioalgo::NodeRegistry& descriptors_ = *descriptors_owner_;
    std::shared_ptr<const NodeRegistry> available_catalog_;
    audioalgo::editor::EditorController controller_;
    const audioalgo::ProjectModel& project_;
    std::filesystem::path project_path_;
    std::filesystem::path output_file_;
    std::uint64_t project_epoch_ = 1;
    audioalgo::RunReception run_reception_;
    RunConfiguration run_configuration_;
    std::shared_ptr<std::atomic<bool>> log_history_cancel_;
    std::uint64_t log_history_generation_=0;
    bool viewing_archived_logs_=false;
    std::shared_ptr<audioalgo::editor::ComponentDispatchState<EditorComponent>> completion_dispatch_state_;
    audioalgo::editor::NodeSelectionState selection_;
    GraphCanvas canvas_;
    NodeInspector inspector_;
    WaveformView waveform_;
    RunLogPanel run_logs_;
    juce::TextButton waveform_tab_{"Waveform"}, logs_tab_{"Run logs"};
    NodeLibrary library_;
    std::shared_ptr<const authoring::DraftStorage> drafts_;
    bool user_draft_files_ = false;
    std::function<void(bool, const std::filesystem::path&, DraftFileActions::Completion)> draft_picker_;
    std::function<void(const std::string&, const std::filesystem::path&, DraftFileActions::Completion)> directory_picker_;
    std::function<void(DraftFileActions::Completion)> editor_picker_;
    std::function<authoring::SaveResult(bool, const std::filesystem::path&)> location_persistence_;
    std::function<authoring::SaveResult(const std::filesystem::path&)> library_location_persistence_;
    bool library_restart_pending_ = false;
    std::unique_ptr<NodeAuthoringPanel> authoring_panel_;
    std::unique_ptr<NodeUpgradePanel> upgrade_panel_;
    std::function<void()> after_draft_close_;
    std::shared_ptr<const authoring::AuthoringChecks> checks_;
    std::uint64_t catalog_generation_ = 0;
    std::uint64_t catalog_resolution_generation_ = 0;
    bool catalog_resolution_pending_ = false;
    std::function<bool()> allow_catalog_prepare_;
    std::unique_ptr<NodeRegistry> prepared_catalog_;
    std::unique_ptr<NodeRegistry> prepared_default_catalog_;
    std::vector<authoring::PackageSelection> prepared_project_locks_;
    std::optional<NodeLibrary::PreparedRefresh> prepared_library_;
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
    bool file_chooser_for_draft_ = false;
    bool run_after_save_ = false;
    bool shutdown_started_ = false;
};


} // namespace audioalgo::editor
