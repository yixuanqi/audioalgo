#pragma once
#include "EditorRunLogPanel.h"
#include "EditorText.h"
#include "EditorUiSupport.h"
#include "EditorPanelSplitter.h"
#include "audio_node_authoring/Draft.h"
#include "audio_node_authoring/Workspace.h"
#include "audio_node_authoring/AuthoringChecks.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <future>
#include <chrono>

namespace audioalgo::editor {
struct DraftFileActions {
    using Completion = std::function<void(std::filesystem::path)>;
    std::function<void(const std::filesystem::path&, Completion)> choose_save;
    std::shared_ptr<const authoring::DraftFiles> documents = std::make_shared<authoring::DraftFiles>();
    std::filesystem::path path;
    std::optional<std::string> expected_bytes;
    std::function<void(const std::filesystem::path&, std::function<void(bool)>)> confirm_overwrite;
    std::function<void(bool, Completion)> choose_directory;
    std::function<authoring::SaveResult(bool, const std::filesystem::path&)> persist_directory;
    std::function<std::string(bool)> directory_override;
    std::function<void(std::shared_ptr<const authoring::AuthoringChecks>, std::shared_ptr<const authoring::DraftStorage>)> locations_changed;
    std::function<void(Completion)> choose_library_directory;
    std::function<authoring::SaveResult(const std::filesystem::path&)> persist_library_directory;
    std::function<bool()> library_change_allowed;
    std::function<bool()> allow_apply;
    std::function<void()> library_change_queued;
    std::string initial_location_message;
    struct WorkspaceBinding {
        std::filesystem::path directory;
        std::string content_sha256;
        std::size_t files = 0;
        std::uint64_t bytes = 0;
    };
    // Read-only directory binding: the directory is authoritative, so this panel
    // holds no second source buffer and never writes a .aadraft for it.
    std::optional<WorkspaceBinding> workspace;
    std::function<void(std::function<void(std::filesystem::path)>)> choose_workspace_directory;
    std::function<authoring::WorkspaceOutcome(const std::filesystem::path&, const authoring::Draft&)> create_workspace;
};
class NodeAuthoringPanel final : public juce::Component, private juce::Timer,
                                 private juce::CodeDocument::Listener {
public:
    NodeAuthoringPanel(authoring::Draft draft, bool saved,
                       std::shared_ptr<const authoring::DraftStorage> storage,
                       std::function<void()> close, std::shared_ptr<const authoring::AuthoringChecks> checks = {},
                       std::function<std::shared_ptr<const NodeRegistry>()> catalog = {},
                       std::function<void(authoring::AppliedPackage)> applied = {},
                       std::function<std::optional<authoring::SourceDiagnostic>(const authoring::AppliedPackage&)> prepare_catalog = {},
                       std::function<void()> discard_catalog = {}, DraftFileActions files = {})
        : session_(std::move(draft), saved), storage_(std::move(storage)), close_(std::move(close)),
          source_(source_document_, nullptr), definition_(definition_document_, nullptr), checks_(std::move(checks)),
          catalog_(std::move(catalog)), applied_(std::move(applied)), prepare_catalog_(std::move(prepare_catalog)), discard_catalog_(std::move(discard_catalog)), files_(std::move(files)) {
        setComponentID("authoring.panel");
        setLookAndFeel(&look_and_feel_);
        definition_splitter_.configure("authoring.splitter.definition", "Resize definition and source", false,
            [this](int delta) { definition_height_ += delta; resized(); },
            [this] { definition_height_ = 160; resized(); });
        logs_splitter_.configure("authoring.splitter.logs", "Resize check logs", false,
            [this](int delta) { check_log_height_ -= delta; resized(); },
            [this] { check_log_height_ = 180; resized(); });
        addAndMakeVisible(definition_splitter_); addAndMakeVisible(logs_splitter_); addAndMakeVisible(trial_hint_);
        trial_hint_.setText("Check calls process() once per audio block; repeated print output is normal.", juce::dontSendNotification);
        trial_hint_.setColour(juce::Label::textColourId, muted); trial_hint_.setFont(juce::FontOptions(12.f));
        file_path_.setMinimumHorizontalScale(1.f); locations_.setMinimumHorizontalScale(1.f);
        for (auto* editor : {&source_, &definition_}) {
            editor->setFont(juce::Font(juce::FontOptions("Consolas", 13.5f, juce::Font::plain)));
            editor->setColour(juce::CodeEditorComponent::backgroundColourId, background);
            editor->setColour(juce::CodeEditorComponent::defaultTextColourId, ink);
            editor->setColour(juce::CodeEditorComponent::lineNumberTextId, muted);
            editor->setColour(juce::CodeEditorComponent::lineNumberBackgroundId, panel);
            editor->setColour(juce::CodeEditorComponent::highlightColourId, selection);
        }
        addAndMakeVisible(check_logs_);
        for (juce::Component* c : std::initializer_list<juce::Component*>{&source_, &definition_, &title_, &source_label_,
                       &definition_label_, &state_, &save_, &close_button_}) addAndMakeVisible(*c);
        title_.setText("Python node", juce::dontSendNotification); title_.setFont(juce::FontOptions(17.f, juce::Font::bold));
        source_label_.setText("Source  /  process(inputs, params, state)", juce::dontSendNotification);
        definition_label_.setText("Definition  /  ports and parameters", juce::dontSendNotification);
        for (auto* label : {&source_label_, &definition_label_}) { label->setFont(juce::FontOptions(12.f, juce::Font::bold)); label->setColour(juce::Label::textColourId, muted); }
        source_.setComponentID("authoring.source"); definition_.setComponentID("authoring.definition");
        addAndMakeVisible(type_id_); addAndMakeVisible(display_name_);
        addAndMakeVisible(templates_); templates_.setComponentID("authoring.template");
        templates_.setTextWhenNothingSelected("Replace with template...");
        templates_.addItem("Stateless gain", 1); templates_.addItem("Stateful ramp", 2);
        templates_.addItem("Named two-input router", 3);
        templates_.onChange = [this] { replace_template(); };
        type_id_.setComponentID("authoring.type_id"); display_name_.setComponentID("authoring.display_name");
        type_id_.setTextToShowWhenEmpty("Logical type ID", muted); display_name_.setTextToShowWhenEmpty("Display name", muted);
        type_id_.setTooltip("Logical type ID (unique in the node catalog)"); display_name_.setTooltip("Name shown in the node library");
        type_id_.onTextChange = display_name_.onTextChange = [this] { definition_fields_changed(); };
        save_.setComponentID("authoring.save"); close_button_.setComponentID("authoring.close"); state_.setComponentID("authoring.status");
        addAndMakeVisible(save_as_); addAndMakeVisible(file_path_);
        save_as_.setComponentID("authoring.save_as"); file_path_.setComponentID("authoring.file_path");
        style_button(save_as_); save_as_.setEnabled(static_cast<bool>(files_.choose_save));
        save_as_.onClick = [this] { request_file_save(true); };
        for (juce::Component* c : std::initializer_list<juce::Component*>{&work_folder_, &recovery_folder_, &library_folder_, &locations_}) addAndMakeVisible(*c);
        work_folder_.setComponentID("authoring.work_folder"); recovery_folder_.setComponentID("authoring.recovery_folder"); locations_.setComponentID("authoring.locations");
        style_button(work_folder_); style_button(recovery_folder_);
        library_folder_.setComponentID("authoring.library_folder"); style_button(library_folder_); library_folder_.onClick = [this] { choose_location(false, true); };
        work_folder_.onClick = [this] { choose_location(false); }; recovery_folder_.onClick = [this] { choose_location(true); };
        locations_.setText(checks_ ? "Library: " + utf8_string(path_to_utf8(checks_->library_directory())) : "Storage locations", juce::dontSendNotification);
        if (!files_.initial_location_message.empty()) locations_.setText(utf8_string(files_.initial_location_message), juce::dontSendNotification);
        locations_.setTooltip(locations_.getText());
        discard_.setComponentID("authoring.discard"); addChildComponent(discard_); style_button(discard_, danger);
        discard_.onClick = [this] { if ((failed_ || (files_.choose_save && session_.dirty())) && !saving_.valid() && !file_save_pending()) finish_close(true); };
        for (juce::Component* c : std::initializer_list<juce::Component*>{&check_, &cancel_, &apply_, &check_state_, &diagnostic_}) addAndMakeVisible(*c);
        check_.setComponentID("authoring.check"); cancel_.setComponentID("authoring.cancel"); diagnostic_.setComponentID("authoring.diagnostic");
        check_state_.setComponentID("authoring.check_status");
        apply_.setComponentID("authoring.apply"); apply_.setEnabled(false); style_button(apply_, signal);
        apply_.onClick = [this] { apply_requested(); };
        check_.setEnabled(static_cast<bool>(checks_)); style_button(check_, signal); style_button(cancel_);
        check_.onClick = [this] { check_requested(); };
        cancel_.onClick = [this] {
            pending_check_.reset(); if (cancel_check_) cancel_check_->store(true);
            if (cancel_apply_) cancel_apply_->store(true);
            ready_.reset(); check_state_.setText(utf8_literal("Draft · check cancelled"), juce::dontSendNotification);
        };
        diagnostic_.onClick = [this] {
            if (diagnostic_line_ > 0) source_.moveCaretTo(juce::CodeDocument::Position(source_document_, diagnostic_line_ - 1, std::max(0, diagnostic_column_ - 1)), false);
        };
        check_state_.setText(checks_ ? utf8_literal("Draft · not checked") : "Checking unavailable in this host", juce::dontSendNotification);
        source_document_.replaceAllContent(utf8_string(session_.document().source));
        definition_document_.replaceAllContent(utf8_string(session_.document().declaration));
        source_document_.clearUndoHistory(); definition_document_.clearUndoHistory();
        source_document_.addListener(this); definition_document_.addListener(this);
        synchronize_definition_fields();
        style_button(save_, signal); style_button(close_button_);
        save_.onClick = [this] { save_requested_ = true; save(); };
        close_button_.onClick = [this] { close_requested_ = true; save_requested_ = true; save(); };
        addAndMakeVisible(workspace_);
        workspace_.setComponentID("authoring.workspace_create");
        workspace_.setTooltip("Create a node project folder from this draft; the .aadraft file is never modified");
        style_button(workspace_);
        workspace_.onClick = [this] { request_workspace_create(); };
        workspace_.setEnabled(!files_.workspace && files_.create_workspace && files_.choose_workspace_directory);
        if (files_.workspace) {
            workspace_directory_ = files_.workspace->directory;
            workspace_content_sha256_ = files_.workspace->content_sha256;
            workspace_file_count_ = files_.workspace->files;
            title_.setText("Node project", juce::dontSendNotification);
            close_button_.setButtonText("Close");
            source_.setReadOnly(true); definition_.setReadOnly(true);
            type_id_.setEnabled(false); display_name_.setEnabled(false); templates_.setEnabled(false);
            check_state_.setText(utf8_literal("Node project · editing the files on disk is authoritative (W2 adds Check)"), juce::dontSendNotification);
        }
        update_status(); startTimer(50);
    }
    ~NodeAuthoringPanel() override {
        stopTimer(); source_document_.removeListener(this); definition_document_.removeListener(this);
        // A save only owns values and storage. It never waits for a message-thread callback.
        if (saving_.valid()) saving_.wait();
        if (file_saving_.valid()) file_saving_.wait();
        if (file_target_loading_.valid()) file_target_loading_.wait();
        if (location_work_.valid()) location_work_.wait();
        if (cancel_check_) cancel_check_->store(true);
        if (checking_.valid()) checking_.wait();
        if (cancel_apply_) cancel_apply_->store(true);
        if (preparing_.valid()) preparing_.wait();
        if (applying_.valid()) applying_.wait();
        if (workspace_creating_.valid()) workspace_creating_.wait();
        if (discard_catalog_) discard_catalog_();
        setLookAndFeel(nullptr);
    }
    bool keyPressed(const juce::KeyPress& key) override {
        if (key.getModifiers().isCtrlDown() || key.getModifiers().isCommandDown()) {
            const auto code = key.getKeyCode();
            auto& document = definition_.hasKeyboardFocus(true) ? definition_document_ : source_document_;
            if (code == 'Z') { if (key.getModifiers().isShiftDown()) document.redo(); else document.undo(); return true; }
            if (code == 'Y') { document.redo(); return true; }
            if (code == 'S') { if (key.getModifiers().isShiftDown() && files_.choose_save) request_file_save(true); else { save_requested_ = true; save(); } return true; }
        }
        return false;
    }
    void paint(juce::Graphics& g) override { g.fillAll(panel); g.setColour(muted.withAlpha(.35f)); g.drawRect(getLocalBounds(), 1); }
    void resized() override {
        auto area = getLocalBounds().reduced(12);
        title_.setBounds(area.removeFromTop(28));
        file_path_.setBounds(area.removeFromTop(22));
        auto locations = area.removeFromTop(28);
        work_folder_.setBounds(locations.removeFromLeft(130).reduced(2)); recovery_folder_.setBounds(locations.removeFromLeft(145).reduced(2)); library_folder_.setBounds(locations.removeFromLeft(135).reduced(2)); locations_.setBounds(locations);
        auto fields = area.removeFromTop(34); templates_.setBounds(fields.removeFromLeft(180).reduced(2));
        type_id_.setBounds(fields.removeFromLeft(fields.getWidth() / 2).reduced(2));
        display_name_.setBounds(fields.reduced(2));
        auto bottom = area.removeFromBottom(42);
        close_button_.setBounds(bottom.removeFromRight(120).reduced(4));
        save_.setBounds(bottom.removeFromRight(120).reduced(4)); state_.setBounds(bottom.reduced(4));
        workspace_.setBounds(bottom.removeFromRight(170).reduced(4));
        save_as_.setBounds(bottom.removeFromRight(110).reduced(4));
        discard_.setBounds(bottom.removeFromRight(140).reduced(4)); state_.setBounds(bottom.reduced(4));
        auto diagnostics = area.removeFromBottom(64);
        auto actions = diagnostics.removeFromTop(32);
        check_.setBounds(actions.removeFromLeft(150).reduced(2)); cancel_.setBounds(actions.removeFromLeft(90).reduced(2));
        apply_.setBounds(actions.removeFromRight(100).reduced(2)); check_state_.setBounds(actions);
        diagnostic_.setBounds(diagnostics);
        const int usable = std::max(248, area.getHeight() - 84);
        definition_height_ = std::clamp(definition_height_, 72, std::max(72, usable - 100 - 76));
        check_log_height_ = std::clamp(check_log_height_, 76, std::max(76, usable - definition_height_ - 100));
        check_logs_.setBounds(area.removeFromBottom(check_log_height_));
        trial_hint_.setBounds(area.removeFromBottom(20));
        logs_splitter_.setBounds(area.removeFromBottom(8));
        auto definition = area.removeFromTop(definition_height_ + 24);
        definition_label_.setBounds(definition.removeFromTop(24)); definition_.setBounds(definition);
        definition_splitter_.setBounds(area.removeFromTop(8));
        source_label_.setBounds(area.removeFromTop(24)); source_.setBounds(area);
    }
    const authoring::DraftSession& session() const { return session_; }
    const std::optional<authoring::ReadyPackage>& ready() const { return ready_; }
    const std::filesystem::path& file_path() const { return files_.path; }
    bool workspace_mode() const { return files_.workspace.has_value(); }
    bool workspace_creating() const { return workspace_creating_.valid(); }
    bool workspace_created() const { return !workspace_directory_.empty(); }
    const std::filesystem::path& workspace_directory() const { return workspace_directory_; }
    const std::string& workspace_content_sha256() const { return workspace_content_sha256_; }
    std::size_t workspace_file_count() const { return workspace_file_count_; }
    bool file_save_pending() const { return choosing_file_ || choosing_overwrite_ || file_target_loading_.valid() || file_saving_.valid(); }
    bool check_in_progress() const { return checking_.valid(); }
    bool check_result_pending() const { return checking_.valid() && checking_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }
    bool apply_in_progress() const { return preparing_.valid() || applying_.valid(); }
    void request_close(std::function<void()> failed = {}) {
        close_failed_ = std::move(failed);
        close_requested_ = true; save_requested_ = true; save();
    }
private:
    static std::uint64_t now() { return static_cast<std::uint64_t>(juce::Time::getMillisecondCounterHiRes()); }
    void codeDocumentTextInserted(const juce::String&, int) override { edited(); }
    void codeDocumentTextDeleted(int, int) override { edited(); }
    void edited() {
        const auto declaration_changed = session_.document().declaration != definition_document_.getAllContent().toStdString();
        session_.edit(source_document_.getAllContent().toStdString(), definition_document_.getAllContent().toStdString(), now());
        if (declaration_changed && !writing_definition_fields_) synchronize_definition_fields();
        failed_ = false; update_status();
        ready_.reset(); diagnostic_line_ = diagnostic_column_ = 0;
        apply_.setEnabled(false); if (cancel_apply_) cancel_apply_->store(true);
        diagnostic_.setButtonText({});
        check_state_.setText(checking_.valid() ? "Checking older draft; edit requires a new check" : utf8_literal("Draft · not checked"), juce::dontSendNotification);
    }
    void synchronize_definition_fields() {
        const auto definition = juce::JSON::parse(definition_document_.getAllContent());
        const auto valid = definition.isObject() && definition["type_id"].isString() && definition["display_name"].isString();
        type_id_.setEnabled(valid); display_name_.setEnabled(valid);
        type_id_.setText(valid ? definition["type_id"].toString() : juce::String{}, false);
        display_name_.setText(valid ? definition["display_name"].toString() : juce::String{}, false);
    }
    void definition_fields_changed() {
        auto definition = juce::JSON::parse(definition_document_.getAllContent());
        if (!definition.isObject()) return;
        definition.getDynamicObject()->setProperty("type_id", type_id_.getText());
        definition.getDynamicObject()->setProperty("display_name", display_name_.getText());
        const juce::ScopedValueSetter<bool> writing(writing_definition_fields_, true);
        definition_document_.replaceAllContent(juce::JSON::toString(definition, false));
    }
    void replace_template() {
        const auto selection = templates_.getSelectedId(); if (selection < 1 || selection > 3) return;
        const auto kind = selection == 3 ? authoring::PythonTemplate::named_router : selection == 2 ? authoring::PythonTemplate::stateful_ramp : authoring::PythonTemplate::stateless_gain;
        auto generated = authoring::python_draft(session_.document().id, kind);
        auto definition = juce::JSON::parse(definition_document_.getAllContent());
        const auto replacement = juce::JSON::parse(utf8_string(generated.declaration));
        if (!definition.isObject()) definition = replacement.clone();
        else for (const auto* field : {"inputs", "outputs", "parameters", "format", "execution", "authoring_template"})
            definition.getDynamicObject()->setProperty(field, replacement[field]);
        source_document_.newTransaction(); definition_document_.newTransaction();
        source_document_.replaceAllContent(utf8_string(generated.source));
        definition_document_.replaceAllContent(juce::JSON::toString(definition, false));
        source_document_.newTransaction(); definition_document_.newTransaction();
        templates_.setSelectedId(0, juce::dontSendNotification);
    }
    void update_status() {
        if (files_.workspace) {
            const auto hash = workspace_content_sha256_.substr(0, std::min<std::size_t>(12, workspace_content_sha256_.size()));
            file_path_.setText("Project: " + utf8_string(path_to_utf8(workspace_directory_)), juce::dontSendNotification);
            file_path_.setTooltip(file_path_.getText());
            state_.setText("Node project · read-only · " + juce::String(static_cast<int>(workspace_file_count_)) +
                " files · " + utf8_string(hash), juce::dontSendNotification);
            state_.setColour(juce::Label::textColourId, failed_ ? danger : muted);
            state_.setTooltip(state_.getText());
            save_.setVisible(false); save_as_.setVisible(false); discard_.setVisible(false); workspace_.setVisible(false);
            check_.setEnabled(false); apply_.setEnabled(false);
            work_folder_.setEnabled(false); recovery_folder_.setEnabled(false); library_folder_.setEnabled(false);
            return;
        }
        const bool user_files = static_cast<bool>(files_.choose_save);
        file_path_.setText(user_files ? (files_.path.empty() ? "File: not saved - choose a location with Save" : "File: " + utf8_string(path_to_utf8(files_.path))) : "Managed draft storage", juce::dontSendNotification);
        file_path_.setTooltip(file_path_.getText());
        state_.setText(failed_ ? utf8_literal("Unsaved · ") + utf8_string(error_) : user_files ?
            (file_save_pending() ? "Saving / choosing file..." : session_.dirty() ? (session_.recovery_current() ? "Recovery backed up; file unsaved" : "Unsaved file") : "File saved") +
                (recovery_error_.empty() ? juce::String{} : " | Recovery unavailable: " + utf8_string(recovery_error_)) :
            saving_.valid() ? utf8_literal("Saving draft…") : session_.dirty() ? "Unsaved draft" : utf8_literal("Draft saved · not applied"), juce::dontSendNotification);
        state_.setColour(juce::Label::textColourId, failed_ ? danger : muted);
        state_.setTooltip(state_.getText());
        const bool can_discard = failed_ || (user_files && session_.dirty());
        discard_.setVisible(can_discard); discard_.setEnabled(can_discard && !saving_.valid() && !file_save_pending());
        save_as_.setEnabled(user_files && !file_save_pending() && !choosing_directory_ && !location_work_.valid());
        const bool can_relocate = files_.choose_directory && !file_save_pending() && !choosing_directory_ && !location_work_.valid() && !checking_.valid() && !apply_in_progress() && !saving_.valid();
        work_folder_.setEnabled(can_relocate && static_cast<bool>(checks_)); recovery_folder_.setEnabled(can_relocate);
        library_folder_.setEnabled(can_relocate && checks_ && files_.choose_library_directory && (!files_.library_change_allowed || files_.library_change_allowed()));
        library_folder_.setTooltip(checks_ ? "Copy all installed versions to an empty folder for the next startup. Current library: " + utf8_string(path_to_utf8(checks_->library_directory())) : "No library configured");
        work_folder_.setTooltip(checks_ ? "Current work directory: " + utf8_string(path_to_utf8(checks_->work_directory())) : "No checker configured");
        recovery_folder_.setTooltip("Current recovery directory: " + utf8_string(path_to_utf8(storage_->directory())));
    }
    void request_workspace_create() {
        if (files_.workspace || workspace_creating_.valid() || !files_.choose_workspace_directory || !files_.create_workspace) return;
        auto safe = juce::Component::SafePointer<NodeAuthoringPanel>(this);
        files_.choose_workspace_directory([safe](std::filesystem::path target) {
            if (safe == nullptr || target.empty()) return;
            const auto draft = safe->session_.document();
            const auto create = safe->files_.create_workspace;
            safe->workspace_creating_ = std::async(std::launch::async, [target, draft, create] { return create(target, draft); });
            safe->update_status();
        });
    }
    void save() {
        if (files_.workspace) { save_requested_ = false; if (close_requested_) finish_close(); return; }
        if (files_.choose_save) { request_file_save(false); return; }
        if (saving_.valid()) return;
        if (!session_.dirty()) { save_requested_ = false; if (close_requested_) finish_close(); return; }
        failed_ = false;
        const auto draft = session_.document(); saving_generation_ = draft.generation;
        saving_ = std::async(std::launch::async, [storage = storage_, draft] { return storage->save(draft); });
        save_requested_ = false; update_status();
    }
    void request_file_save(bool as) {
        if (!files_.choose_save || file_save_pending()) return;
        if (choosing_directory_ || location_work_.valid()) { save_requested_ = true; return; }
        save_requested_ = false;
        if (!as && !files_.path.empty() && !session_.dirty()) { if (close_requested_) finish_close(); return; }
        if (!as && !files_.path.empty()) { start_file_save(files_.path, files_.expected_bytes); return; }
        choosing_file_ = true;
        const auto receipt = ++file_request_;
        auto safe = juce::Component::SafePointer<NodeAuthoringPanel>(this);
        files_.choose_save(files_.path, [safe, receipt](std::filesystem::path path) {
            if (!safe || safe->file_request_ != receipt) return;
            ++safe->file_request_;
            safe->choosing_file_ = false;
            if (path.empty()) {
                safe->save_requested_ = false; safe->close_requested_ = false;
                auto failed = std::move(safe->close_failed_); if (failed) failed();
                safe->update_status(); return;
            }
            safe->prepare_file_target(std::move(path));
        });
        update_status();
    }
    void prepare_file_target(std::filesystem::path path) {
        if (path == files_.path) { start_file_save(std::move(path), files_.expected_bytes); return; }
        failed_ = false; file_target_ = path;
        file_target_loading_ = std::async(std::launch::async, [documents = files_.documents, path] { return documents->inspect_target(path); });
        update_status();
    }
    void start_file_save(std::filesystem::path path, std::optional<std::string> expected) {
        failed_ = false; file_target_ = std::move(path);
        const auto draft = session_.document();
        const auto target = file_target_;
        file_saving_ = std::async(std::launch::async, [documents = files_.documents, draft, target, expected = std::move(expected)] {
            return documents->save(target, draft, expected);
        });
        update_status();
    }
    void finish_close(bool discard = false) {
        discard_requested_ = discard_requested_ || discard;
        if (choosing_directory_ || location_work_.valid()) { close_requested_ = true; return; }
        if (file_save_pending()) { close_requested_ = true; return; }
        if (saving_.valid()) { close_requested_ = true; return; }
        if (apply_in_progress()) { if (cancel_apply_) cancel_apply_->store(true); close_requested_ = true; return; }
        if (checking_.valid()) { pending_check_.reset(); if (cancel_check_) cancel_check_->store(true); close_requested_ = true; return; }
        discard = discard_requested_;
        close_requested_ = false;
        // Defer destruction until after this timer/button callback has returned.
        auto safe = juce::Component::SafePointer<NodeAuthoringPanel>(this);
        juce::MessageManager::callAsync([safe, discard] {
            if (safe == nullptr) return;
            if (!discard && (safe->session_.dirty() || safe->saving_.valid() || safe->file_save_pending())) { safe->request_close(); return; }
            // Calling a member std::function that destroys its owner would also
            // destroy its live closure. Move it out before releasing the panel.
            auto callback = std::move(safe->close_); if (callback) callback();
        });
    }
    void timerCallback() override {
        if (workspace_creating_.valid() && workspace_creating_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto outcome = workspace_creating_.get();
            if (auto* error = std::get_if<Diagnostic>(&outcome)) {
                failed_ = true; error_ = error->code + ": " + error->message;
            } else {
                auto created = std::get<authoring::Workspace>(std::move(outcome));
                workspace_directory_ = std::move(created.directory);
                workspace_content_sha256_ = std::move(created.content_sha256);
                workspace_file_count_ = created.files.size();
                failed_ = false; error_.clear();
                locations_.setText(utf8_string("Node project: " + path_to_utf8(workspace_directory_) + " | " +
                    std::to_string(workspace_file_count_) + " files | " + workspace_content_sha256_.substr(0, 12)),
                    juce::dontSendNotification);
                locations_.setTooltip(locations_.getText());
                check_state_.setText(utf8_literal("Node project created - reopen the folder to verify declaration and source"),
                    juce::dontSendNotification);
            }
            update_status();
        }
        if (file_target_loading_.valid() && file_target_loading_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto outcome = file_target_loading_.get();
            if (auto* error = std::get_if<Diagnostic>(&outcome)) {
                failed_ = true; error_ = error->code + ": " + error->message; close_requested_ = false; save_requested_ = false;
                auto failed = std::move(close_failed_); if (failed) failed();
            } else if (std::holds_alternative<std::monostate>(outcome)) start_file_save(file_target_, {});
            else if (!files_.confirm_overwrite) {
                failed_ = true; error_ = "Existing file preserved. Choose another filename or use a host with overwrite confirmation.";
                close_requested_ = false; save_requested_ = false;
                auto failed = std::move(close_failed_); if (failed) failed();
            } else {
                choosing_overwrite_ = true;
                const auto receipt = ++file_request_;
                auto safe = juce::Component::SafePointer<NodeAuthoringPanel>(this);
                files_.confirm_overwrite(file_target_, [safe, receipt, expected = std::get<authoring::DraftFile>(std::move(outcome)).bytes](bool replace) {
                    if (!safe || safe->file_request_ != receipt) return;
                    ++safe->file_request_; safe->choosing_overwrite_ = false;
                    if (replace) safe->start_file_save(safe->file_target_, expected);
                    else {
                        safe->close_requested_ = false; safe->save_requested_ = false;
                        auto failed = std::move(safe->close_failed_); if (failed) failed();
                    }
                    safe->update_status();
                });
            }
            update_status();
        }
        if (location_work_.valid() && location_work_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = location_work_.get();
            if (result.success) {
                if (result.library) { if (files_.library_change_queued) files_.library_change_queued(); }
                else if (result.recovery) { storage_ = result.storage; recovery_error_.clear(); session_.invalidate_recovery(); }
                else { checks_ = result.checks; ready_.reset(); check_state_.setText("Work folder changed - check again", juce::dontSendNotification); }
                if (!result.library && files_.locations_changed) files_.locations_changed(checks_, storage_);
            }
            locations_.setText(utf8_string(result.message), juce::dontSendNotification); locations_.setTooltip(locations_.getText());
            update_status();
        }
        if (file_saving_.valid() && file_saving_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = file_saving_.get();
            if (auto* error = std::get_if<Diagnostic>(&result)) {
                failed_ = true; error_ = error->code + ": " + error->message;
                close_requested_ = false; save_requested_ = false;
                auto failed = std::move(close_failed_); if (failed) failed();
            } else {
                auto saved = std::get<authoring::DraftFile>(std::move(result));
                files_.path = file_target_; files_.expected_bytes = std::move(saved.bytes);
                session_.saved(saved.document.generation); session_.invalidate_recovery(); recovery_error_.clear();
            }
            update_status();
        }
        if (preparing_.valid() && preparing_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto outcome = preparing_.get();
            std::optional<authoring::SourceDiagnostic> error;
            if (auto* failed = std::get_if<authoring::SourceDiagnostic>(&outcome)) error = *failed;
            else if (cancel_apply_->load() || applying_draft_ != session_.document())
                error = authoring::SourceDiagnostic{"run_cancelled", "Draft changed or Apply cancelled before refresh acceptance"};
            else {
                auto prepared = std::get<authoring::PreparedPackage>(std::move(outcome));
                try { if (prepare_catalog_) error = prepare_catalog_(prepared.package); }
                catch (const std::exception& failure) { error = authoring::SourceDiagnostic{"authoring_catalog_refresh_failed", failure.what()}; }
                if (!error) applying_ = std::async(std::launch::async, [transaction = std::move(prepared.transaction), cancel = cancel_apply_] {
                    return transaction->commit(cancel);
                });
            }
            if (error) apply_failed(*error);
        }
        if (applying_.valid() && applying_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto outcome = applying_.get();
            if (auto* result = std::get_if<authoring::AppliedPackage>(&outcome)) {
                // A committed catalog remains authoritative even if the draft
                // was edited during installation. No project mutation occurs.
                if (applied_) applied_(std::move(*result));
                if (applying_draft_ == session_.document()) check_state_.setText("Available in node library", juce::dontSendNotification);
            } else apply_failed(std::get<authoring::SourceDiagnostic>(outcome));
        }
        if (close_requested_ && !apply_in_progress() && !checking_.valid()) {
            if (discard_requested_) finish_close(true);
            else if (!failed_) { if (session_.dirty() || saving_.valid()) save_requested_ = true; else finish_close(); }
        }
        if (checking_.valid() && checking_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto outcome = checking_.get();
            // Cancellation revokes delivery even when the worker completed before
            // the message thread observed the result.
            if (!cancel_check_->load() && checking_draft_ == session_.document()) {
                const auto& logs = std::holds_alternative<authoring::ReadyPackage>(outcome)
                    ? std::get<authoring::ReadyPackage>(outcome).logs : std::get<authoring::SourceDiagnostic>(outcome).logs;
                for (const auto& row : logs) if (row.draft_id == session_.document().id && row.generation == session_.document().generation) {
                    auto event = row.event;
                    event.text = "draft=" + row.draft_id + " generation=" + std::to_string(row.generation) +
                        " source=" + row.source_sha256 + " declaration=" + row.declaration_sha256 + " phase=" + row.phase + "\n" + event.text;
                    check_logs_.append(event);
                }
                if (auto* result = std::get_if<authoring::ReadyPackage>(&outcome)) {
                    if (result->draft == session_.document()) { ready_ = std::move(*result); check_state_.setText(utf8_literal("Ready · contract check passed"), juce::dontSendNotification); }
                } else {
                    auto error = std::get<authoring::SourceDiagnostic>(outcome); ready_.reset();
                    diagnostic_line_ = error.line; diagnostic_column_ = error.column;
                    const auto position = error.file.empty() || error.line <= 0 ? std::string{} : error.file + ":" + std::to_string(error.line) + ":" + std::to_string(error.column) + " ";
                    diagnostic_.setButtonText(utf8_string(error.code + ": " + position + error.message));
                    diagnostic_.setTooltip(diagnostic_.getButtonText());
                    check_state_.setText(utf8_literal(error.code == "run_cancelled" ? "Draft · check cancelled" : "Draft · check failed"), juce::dontSendNotification);
                }
            }
            if (pending_check_) { auto pending = std::move(*pending_check_); pending_check_.reset(); if (pending == session_.document()) start_check(std::move(pending)); }
            if (close_requested_ && !checking_.valid()) {
                if (discard_requested_) finish_close(true);
                else if (session_.dirty()) save_requested_ = true;
                else finish_close();
            }
        }
        if (saving_.valid() && saving_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = saving_.get();
            if (files_.choose_save) {
                if (result) recovery_error_ = result->message;
                else {
                    recovery_error_.clear();
                    if (recovery_path_ == files_.path && recovery_expected_ == files_.expected_bytes) session_.recovered(saving_generation_);
                }
            } else if (result) {
                failed_ = true; error_ = result->message; close_requested_ = false; save_requested_ = false;
                auto failed = std::move(close_failed_); if (failed) failed();
            }
            else session_.saved(saving_generation_);
            update_status();
            if (!failed_ && close_requested_) { if (session_.dirty()) save_requested_ = true; else finish_close(); }
        }
        if (files_.workspace) {
            // Directory mode never writes a .aadraft or a recovery copy: the
            // directory bytes are the only authority (ADR0040 §8).
        } else if (files_.choose_save) {
            if (!failed_ && save_requested_) save();
            if (!saving_.valid() && recovery_error_.empty() && session_.recovery_due(now())) {
                const auto draft = session_.document(); saving_generation_ = draft.generation;
                recovery_path_ = files_.path; recovery_expected_ = files_.expected_bytes;
                saving_ = std::async(std::launch::async, [storage = storage_, draft, path = files_.path, expected = files_.expected_bytes] { return storage->save_recovery(draft, path, expected); });
            }
            update_status();
        } else if (!failed_ && (save_requested_ || session_.save_due(now()))) save();
        check_.setEnabled(!files_.workspace && checks_ && !apply_in_progress() && !location_work_.valid() && !choosing_directory_);
        apply_.setEnabled(!files_.workspace && ready_ && ready_->draft == session_.document() && !checking_.valid() && !apply_in_progress() && !location_work_.valid() && !choosing_directory_ && (!files_.allow_apply || files_.allow_apply()) && catalog_ && applied_);
    }
    void apply_failed(const authoring::SourceDiagnostic& error) {
        if (discard_catalog_) discard_catalog_();
        if (applying_draft_ != session_.document()) return;
        diagnostic_.setButtonText(utf8_string(error.code + ": " + error.message));
        check_state_.setText(utf8_literal("Draft · apply failed"), juce::dontSendNotification);
    }
    void apply_requested() {
        if (files_.allow_apply && !files_.allow_apply()) { check_state_.setText("Library move queued - save and restart before applying", juce::dontSendNotification); return; }
        if (!ready_ || ready_->draft != session_.document() || apply_in_progress() || checking_.valid() || location_work_.valid() || choosing_directory_ || !catalog_ || !applied_) return;
        auto base = catalog_(); if (!base) return;
        applying_draft_ = session_.document(); cancel_apply_ = std::make_shared<std::atomic<bool>>(false);
        check_state_.setText(utf8_literal("Applying · immutable version"), juce::dontSendNotification); apply_.setEnabled(false); check_.setEnabled(false);
        preparing_ = std::async(std::launch::async, [service = checks_, ready = *ready_, draft = applying_draft_, base = std::move(base), cancel = cancel_apply_] {
            return service->prepare_apply(ready, draft, base, cancel);
        });
    }
    void check_requested() {
        if (!checks_ || apply_in_progress() || location_work_.valid() || choosing_directory_) return;
        if (checking_.valid()) { pending_check_ = session_.document(); cancel_check_->store(true); return; }
        start_check(session_.document());
    }
    void start_check(authoring::Draft draft) {
        check_logs_.clear();
        ready_.reset(); diagnostic_.setButtonText({}); diagnostic_line_ = diagnostic_column_ = 0;
        checking_draft_ = draft; cancel_check_ = std::make_shared<std::atomic<bool>>(false);
        check_state_.setText(utf8_literal("Checking · isolated Python process"), juce::dontSendNotification);
        checking_ = std::async(std::launch::async, [service = checks_, draft = std::move(draft), cancel = cancel_check_]() mutable {
            return service->check(std::move(draft), cancel);
        });
    }
    struct LocationResult {
        bool success = false, recovery = false, library = false;
        std::shared_ptr<const authoring::AuthoringChecks> checks;
        std::shared_ptr<const authoring::DraftStorage> storage;
        std::string message;
    };
    void choose_location(bool recovery, bool library = false) {
        if (!files_.choose_directory || file_save_pending() || checking_.valid() || apply_in_progress() || saving_.valid() || location_work_.valid() || choosing_directory_) return;
        if (library && (!files_.choose_library_directory || (files_.library_change_allowed && !files_.library_change_allowed()))) return;
        const auto override = library ? juce::SystemStats::getEnvironmentVariable("AUDIOALGO_AUTHORING_ROOT", {}).toStdString() : files_.directory_override ? files_.directory_override(recovery) : std::string{};
        if (!override.empty()) { locations_.setText("Environment override: " + utf8_string(override), juce::dontSendNotification); locations_.setTooltip(locations_.getText()); return; }
        choosing_directory_ = true;
        auto safe = juce::Component::SafePointer<NodeAuthoringPanel>(this);
        auto selected = [safe, recovery, library](std::filesystem::path path) {
            if (!safe) return;
            safe->choosing_directory_ = false;
            if (path.empty()) { safe->update_status(); return; }
            safe->location_work_ = std::async(std::launch::async, [path, recovery, library, checks = safe->checks_, persist = safe->files_.persist_directory, persist_library = safe->files_.persist_library_directory] {
                LocationResult result; result.recovery = recovery; result.library = library;
                try {
                if (library) {
                    if (!checks) { result.message = "No library configured"; return result; }
                    if (auto error = checks->copy_library_to(path)) { result.message = error->code + ": " + error->message; return result; }
                    if (!persist_library) { result.message = "Library copied and verified, but next-startup settings are unavailable; current library unchanged"; return result; }
                    if (auto error = persist_library(path)) { result.message = "Library copied and verified; settings not saved, current library unchanged: " + error->message; return result; }
                    result.success = true; result.message = "Library verified for next startup: " + path_to_utf8(path) + " | Current library remains active; save before closing"; return result;
                }
                if (auto error = authoring::probe_authoring_directory(path)) { result.message = error->code + ": " + error->message; return result; }
                if (recovery) result.storage = std::make_shared<authoring::FileDraftStorage>(path);
                else { result.checks = checks ? checks->with_work_directory(path) : nullptr; if (!result.checks) { result.message = "This checker cannot change its work directory"; return result; } }
                result.success = true;
                result.message = std::string(recovery ? "Recovery: " : "Work: ") + path_to_utf8(path);
                if (persist) { if (auto error = persist(recovery, path)) result.message += " | Session only; settings not saved: " + error->message; }
                else result.message += " | Session only";
                return result;
                } catch (const std::exception& error) { result.success = false; result.message = std::string("Directory change failed; current settings preserved: ") + error.what(); return result; }
            });
            safe->update_status();
        };
        if (library) files_.choose_library_directory(std::move(selected));
        else files_.choose_directory(recovery, std::move(selected));
        update_status();
    }
    EditorLookAndFeel look_and_feel_;
    PanelSplitter definition_splitter_, logs_splitter_;
    int definition_height_ = 160, check_log_height_ = 180;
    juce::Label trial_hint_;
    authoring::DraftSession session_;
    std::shared_ptr<const authoring::DraftStorage> storage_;
    std::function<void()> close_;
    std::function<void()> close_failed_;
    juce::CodeDocument source_document_, definition_document_;
    juce::CodeEditorComponent source_, definition_;
    juce::TextEditor type_id_, display_name_;
    juce::ComboBox templates_;
    bool writing_definition_fields_ = false;
    juce::Label title_, source_label_, definition_label_, state_;
    juce::TextButton save_{"Save draft"}, close_button_{"Save && close"};
    juce::TextButton discard_{"Discard changes"};
    std::future<authoring::SaveResult> saving_;
    std::uint64_t saving_generation_ = 0;
    bool failed_ = false, close_requested_ = false, save_requested_ = false, discard_requested_ = false;
    std::string error_;
    DraftFileActions files_;
    juce::TextButton save_as_{"Save as..."};
    juce::TextButton workspace_{"Node project folder..."};
    std::future<authoring::WorkspaceOutcome> workspace_creating_;
    std::filesystem::path workspace_directory_;
    std::string workspace_content_sha256_;
    std::size_t workspace_file_count_ = 0;
    juce::Label file_path_;
    std::future<authoring::DraftFileOutcome> file_saving_;
    std::future<authoring::DraftTargetOutcome> file_target_loading_;
    std::filesystem::path file_target_;
    bool choosing_file_ = false;
    bool choosing_overwrite_ = false;
    std::uint64_t file_request_ = 0;
    std::string recovery_error_;
    std::filesystem::path recovery_path_;
    std::optional<std::string> recovery_expected_;
    juce::TextButton work_folder_{"Work folder..."}, recovery_folder_{"Recovery folder..."}, library_folder_{"Library folder..."};
    juce::Label locations_;
    std::future<LocationResult> location_work_;
    bool choosing_directory_ = false;
    std::shared_ptr<const authoring::AuthoringChecks> checks_;
    std::shared_ptr<std::atomic<bool>> cancel_check_;
    std::future<authoring::CheckOutcome> checking_;
    authoring::Draft checking_draft_;
    std::optional<authoring::Draft> pending_check_;
    std::optional<authoring::ReadyPackage> ready_;
    std::function<std::shared_ptr<const NodeRegistry>()> catalog_;
    std::function<void(authoring::AppliedPackage)> applied_;
    std::function<std::optional<authoring::SourceDiagnostic>(const authoring::AppliedPackage&)> prepare_catalog_;
    std::function<void()> discard_catalog_;
    std::future<authoring::ApplyOutcome> applying_;
    std::future<authoring::PrepareOutcome> preparing_;
    std::shared_ptr<std::atomic<bool>> cancel_apply_;
    authoring::Draft applying_draft_;
    int diagnostic_line_ = 0, diagnostic_column_ = 0;
    juce::TextButton check_{"Check / trial run"}, cancel_{"Cancel"}, apply_{"Apply"}, diagnostic_;
    juce::Label check_state_;
    RunLogPanel check_logs_{"authoring.check_logs", "Check"};
};
}
