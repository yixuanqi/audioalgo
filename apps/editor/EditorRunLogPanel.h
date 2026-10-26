#pragma once
#include "EditorUiSupport.h"
#include "EditorRunLogArchive.h"
#include "audio_run_api/NodeLogEvent.h"
#include "audio_core/Utf8LogDecoder.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <deque>
#include <map>
#include <optional>
#include <string>

namespace audioalgo::editor {
// CodeEditor lays out visible lines rather than shaping the whole retained log
// on every append. No tokeniser, user-code execution or file access is involved.
class RunLogTextView final : public juce::Component {
public:
    RunLogTextView() {
        addAndMakeVisible(editor_); editor_.setReadOnly(true); editor_.setLineNumbersShown(false);
        editor_.setColour(juce::CodeEditorComponent::backgroundColourId, panel);
        editor_.setColour(juce::CodeEditorComponent::defaultTextColourId, ink);
    }
    juce::String getText() const { return document_.getAllContent(); }
    juce::String getHighlightedText() const {
        const auto range = editor_.getHighlightedRegion();
        return document_.getTextBetween(juce::CodeDocument::Position(document_, range.getStart()),
                                        juce::CodeDocument::Position(document_, range.getEnd()));
    }
    void clear() { editor_.loadContent({}); }
    void setText(const juce::String& text, bool) { editor_.loadContent(text); }
    void insertTextAtCaret(const juce::String& text) {
        document_.insertText(document_.getNumCharacters(), text); document_.clearUndoHistory();
    }
    void moveCaretToEnd() { editor_.moveCaretTo(juce::CodeDocument::Position(document_, document_.getNumCharacters()), false); }
    void resized() override { editor_.setBounds(getLocalBounds()); }
private:
    mutable juce::CodeDocument document_;
    juce::CodeEditorComponent editor_{document_, nullptr};
};

class RunLogPanel final : public juce::Component {
public:
    explicit RunLogPanel(const juce::String& id = "project.run_logs", std::string heading = "Run") : heading_(std::move(heading)) {
        addAndMakeVisible(text_); addAndMakeVisible(copy_); addAndMakeVisible(node_filter_); addAndMakeVisible(level_filter_);
        text_.setComponentID(id);
        copy_.setComponentID(id + ".copy"); style_button(copy_);
        node_filter_.setComponentID(id + ".node_filter");
        node_filter_.setColour(juce::ComboBox::backgroundColourId, panel);
        node_filter_.setColour(juce::ComboBox::textColourId, ink);
        node_filter_.setColour(juce::ComboBox::outlineColourId, muted);
        node_filter_.setColour(juce::ComboBox::arrowColourId, signal);
        node_filter_.onChange = [this] { change_node_filter(); };
        level_filter_.setComponentID(id + ".level_filter");
        level_filter_.setColour(juce::ComboBox::backgroundColourId, panel);
        level_filter_.setColour(juce::ComboBox::textColourId, ink);
        level_filter_.setColour(juce::ComboBox::outlineColourId, muted);
        level_filter_.setColour(juce::ComboBox::arrowColourId, signal);
        level_filter_.onChange = [this] {
            const auto found = level_filter_ids_.find(level_filter_.getSelectedId());
            selected_level_ = found == level_filter_ids_.end() ? std::nullopt : std::optional<std::string>(found->second);
            rebuild_display();
        };
        copy_.onClick = [this] { juce::SystemClipboard::copyTextToClipboard(text_for_copy()); };
        addChildComponent(history_);addChildComponent(history_refresh_);addChildComponent(history_status_);
        history_.setComponentID(id+".history");history_refresh_.setComponentID(id+".history_refresh");history_status_.setComponentID(id+".history_status");
        style_button(history_refresh_);
        history_refresh_.setTooltip("Browse recent logged Runs for this project");
        history_refresh_.onClick=[this]{if(refresh_history_)refresh_history_();};
        history_.onChange=[this]{if(select_history_){const auto index=history_.getSelectedId()-2;select_history_(index>=0&&static_cast<std::size_t>(index)<archives_.size()?std::optional<RunLogArchiveReference>(archives_[index]):std::nullopt);}};
        rebuild_filter_items();
    }
    void clear() {
        clear_entries();archives_.clear();history_.clear(juce::dontSendNotification);history_.addItem("Current Run",1);history_.setSelectedId(1,juce::dontSendNotification);set_history_busy(false);history_status_.setText({},juce::dontSendNotification);
    }
    void clear_entries() {
        entries_.clear(); retained_bytes_ = 0; displayed_bytes_.clear(); displayed_context_.clear();
        selected_node_.reset(); selected_level_.reset(); text_.clear(); rebuild_filter_items();
    }
    void append(const NodeLogEvent& event) {
        append_event(event, true);
    }
    // A mailbox drain is one synchronous display transaction. Retain/filter
    // every event, then lay out the bounded final text once before returning.
    void append_batch(const std::deque<NodeLogEvent>& events) {
        if (events.empty()) return;
        for (const auto& event : events) append_event(event, false);
        rebuild_filter_items();
        rebuild_display();
    }
private:
    void append_event(const NodeLogEvent& event, bool render) {
        auto header = "[" + heading_ + " " + event.identity.request_id + " project=" + event.identity.project_id +
            " session=" + std::to_string(event.identity.session_epoch) + " revision=" + std::to_string(event.identity.revision) +
            " node=" + event.node_id + " type=" + event.type_id + " version=" + event.version +
            " package=" + event.package_id + " hash=" + event.content_sha256 +
            " " + event.source + "/" + event.level + "]\n";
        if (!event.report_file.empty()) header += "Report: " + event.report_file + "\n";
        if (!event.raw_log_file.empty()) header += "Full stream: " + event.raw_log_file + "\n";

        // Escape control bytes before crossing JUCE's NUL-terminated text
        // boundary. Raw events/files retain their exact borrowed byte lengths.
        std::string readable;readable.reserve(event.text.size());
        constexpr char hex[]="0123456789abcdef";
        for(const unsigned char c:event.text) {
            if(c==0)readable+="\\0";
            else if((c<0x20 && c!='\r' && c!='\n' && c!='\t') || c==0x7f) {
                readable+="\\x";readable+=hex[c>>4];readable+=hex[c&15];
            } else readable+=static_cast<char>(c);
        }
        std::string value = utf8_string(readable).replace("\r\n", "\n").toStdString();
        const bool has_marker = event.discarded_bytes || event.dropped_text_bytes || event.ui_dropped_events || event.ui_dropped_text_bytes;
        if (has_marker && !value.empty() && value.back() != '\n') value += '\n';
        if (event.discarded_bytes) value += "[stream tail discarded " + std::to_string(event.discarded_bytes) + " bytes]\n";
        if (event.dropped_text_bytes) value += "[display stream dropped " + std::to_string(event.dropped_text_bytes) + " UTF-8 bytes; full stream archived]\n";
        if (event.ui_dropped_events || event.ui_dropped_text_bytes) value += "[UI queue dropped " + std::to_string(event.ui_dropped_events) + " events / " +
            std::to_string(event.ui_dropped_text_bytes) + " text bytes; full stream archived]\n";

        entries_.push_back({event.node_id, event.level, std::move(header), std::move(value)});
        retained_bytes_ += entries_.back().context.size() + entries_.back().text.size() + 1;
        if (!node_filter_item_ids_.contains(event.node_id)) add_node_filter_item(event.node_id);
        if (!level_filter_item_ids_.contains(event.level)) add_level_filter_item(event.level);
        if (trim_tail()) {
            if (render) { rebuild_filter_items(); rebuild_display(); }
            return;
        }
        const auto& entry = entries_.back();
        if (render && matches_filter(entry))
            append_to_display(entry);
    }
public:
    void resized() override {
        const auto available = std::max(0, getWidth() - 100);
        const auto level_width = std::min(140, available / 2);
        const auto node_width = std::min(260, std::max(0, available - level_width - 6));
        node_filter_.setBounds(0, 0, node_width, 24);
        level_filter_.setBounds(node_width + 6, 0, level_width, 24);
        copy_.setBounds(getWidth() - 90, 0, 86, 24);
        const auto top=refresh_history_?56:28;
        history_.setBounds(0,28,std::min(480,std::max(0,getWidth()-100)),24);
        history_refresh_.setBounds(getWidth()-90,28,86,24);
        history_status_.setBounds(history_.getRight()+6,28,std::max(0,history_refresh_.getX()-history_.getRight()-12),24);
        text_.setBounds(0, top, getWidth(), std::max(0, getHeight() - top));
    }
    juce::String text_for_copy() const {
        return text_.getHighlightedText().isEmpty() ? utf8_string(displayed_bytes_) : text_.getHighlightedText();
    }
    void enable_history(std::function<void()> refresh,std::function<void(std::optional<RunLogArchiveReference>)> select) {
        refresh_history_=std::move(refresh);select_history_=std::move(select);
        history_.setVisible(true);history_refresh_.setVisible(true);history_status_.setVisible(true);clear();resized();
    }
    void set_history_list(RunLogArchiveList list) {
        archives_=std::move(list.runs);history_.clear(juce::dontSendNotification);history_.addItem("Current Run",1);
        for(std::size_t i=0;i<archives_.size();++i)history_.addItem(juce::Time(static_cast<juce::int64>(archives_[i].received_unix_ms)).toString(true,true)+" · "+utf8_string(archives_[i].identity.request_id)+" · revision "+juce::String(static_cast<juce::int64>(archives_[i].identity.revision)),static_cast<int>(i)+2);
        history_.setSelectedId(1,juce::dontSendNotification);set_history_status(list.notice);
    }
    void set_history_busy(bool busy) {history_.setEnabled(!busy);history_refresh_.setEnabled(!busy);if(busy)set_history_status("Loading log history…");}
    void set_history_status(const std::string& text) {history_status_.setText(utf8_string(text),juce::dontSendNotification);history_status_.setTooltip(utf8_string(text));}

private:
    struct Entry { std::string node_id, level, context, text; };
    static constexpr std::size_t display_limit = 256 * 1024;
    static constexpr std::size_t trim_target = 192 * 1024;

    void rebuild_filter_items() {
        rebuild_level_items();
        const auto retained_selection = selected_node_;
        node_filter_.clear(juce::dontSendNotification);
        node_filter_.addItem("All nodes", 1);
        node_filter_ids_.clear();
        node_filter_item_ids_.clear();
        next_filter_item_id_ = 2;
        std::map<std::string, std::string> nodes;
        for (const auto& entry : entries_)
            nodes.try_emplace(entry.node_id, entry.node_id.empty() ? "(no node id)" : entry.node_id);
        for (const auto& [node_id, label] : nodes) add_node_filter_item(node_id, label);
        if (retained_selection && nodes.contains(*retained_selection)) {
            const auto found = std::find_if(node_filter_ids_.begin(), node_filter_ids_.end(), [&](const auto& item) {
                return item.second == *retained_selection;
            });
            if (found != node_filter_ids_.end()) {
                node_filter_.setSelectedId(found->first, juce::dontSendNotification);
                return;
            }
        }
        selected_node_.reset();
        node_filter_.setSelectedId(1, juce::dontSendNotification);
    }
    void add_node_filter_item(const std::string& node_id, const std::string& label = {}) {
        const int item_id = next_filter_item_id_++;
        node_filter_ids_.emplace(item_id, node_id);
        node_filter_item_ids_.emplace(node_id, item_id);
        node_filter_.addItem(utf8_string(label.empty() ? (node_id.empty() ? "(no node id)" : node_id) : label), item_id);
    }
    void add_level_filter_item(const std::string& level) {
        const int id = static_cast<int>(level_filter_ids_.size()) + 2;
        level_filter_ids_.emplace(id, level); level_filter_item_ids_.emplace(level, id);
        level_filter_.addItem(utf8_string(level.empty() ? "(no level)" : level), id);
    }
    void rebuild_level_items() {
        level_filter_.clear(juce::dontSendNotification); level_filter_.addItem("All levels", 1);
        level_filter_ids_.clear(); level_filter_item_ids_.clear();
        for (const auto& entry : entries_)
            if (!level_filter_item_ids_.contains(entry.level)) add_level_filter_item(entry.level);
        if (selected_level_ && level_filter_item_ids_.contains(*selected_level_))
            level_filter_.setSelectedId(level_filter_item_ids_.at(*selected_level_), juce::dontSendNotification);
        else { selected_level_.reset(); level_filter_.setSelectedId(1, juce::dontSendNotification); }
    }
    bool matches_filter(const Entry& entry) const {
        return (!selected_node_ || *selected_node_ == entry.node_id) && (!selected_level_ || *selected_level_ == entry.level);
    }
    void change_node_filter() {
        const auto selected_id = node_filter_.getSelectedId();
        if (selected_id == 1) {
            selected_node_.reset();
        } else if (const auto found = node_filter_ids_.find(selected_id); found != node_filter_ids_.end()) {
            selected_node_ = found->second;
        } else {
            selected_node_.reset();
        }
        rebuild_display();
    }
    bool trim_tail() {
        if (retained_bytes_ <= display_limit) return false;
        while (entries_.size() > 1 && retained_bytes_ > trim_target) {
            retained_bytes_ -= entries_.front().context.size() + entries_.front().text.size() + 1;
            entries_.pop_front();
        }
        if (retained_bytes_ > display_limit && !entries_.empty()) {
            auto& entry = entries_.front();
            const auto context_limit = std::min(entry.context.size(), display_limit / 2);
            if (entry.context.size() > context_limit) audioalgo::trim_log_text(entry.context, context_limit);
            const auto text_limit = display_limit > entry.context.size() ? display_limit - entry.context.size() : 0;
            audioalgo::trim_log_text(entry.text, text_limit);
            retained_bytes_ = entry.context.size() + entry.text.size() + 1;
        }
        return true;
    }
    void append_to_display(const Entry& entry) {
        std::string added;
        if (displayed_context_ != entry.context) {
            if (!displayed_bytes_.empty() && displayed_bytes_.back() != '\n') added = "\n";
            added += entry.context;
            displayed_context_ = entry.context;
        }
        added += entry.text;
        displayed_bytes_ += added;
        if (displayed_bytes_.size() > display_limit) audioalgo::trim_log_text(displayed_bytes_, display_limit);
        text_.moveCaretToEnd();
        text_.insertTextAtCaret(utf8_string(added));
        text_.moveCaretToEnd();
    }
    void rebuild_display() {
        displayed_bytes_.clear(); displayed_context_.clear();
        for (const auto& entry : entries_)
            if (matches_filter(entry))
                append_to_display_buffer(entry);
        if (displayed_bytes_.size() > display_limit) audioalgo::trim_log_text(displayed_bytes_, display_limit);
        text_.setText(utf8_string(displayed_bytes_), false);
        text_.moveCaretToEnd();
    }
    void append_to_display_buffer(const Entry& entry) {
        if (displayed_context_ != entry.context) {
            if (!displayed_bytes_.empty() && displayed_bytes_.back() != '\n') displayed_bytes_ += '\n';
            displayed_bytes_ += entry.context;
            displayed_context_ = entry.context;
        }
        displayed_bytes_ += entry.text;
    }

    std::deque<Entry> entries_;
    std::size_t retained_bytes_ = 0;
    std::string displayed_bytes_;
    std::string displayed_context_;
    std::string heading_;
    std::optional<std::string> selected_node_;
    std::optional<std::string> selected_level_;
    std::map<int, std::string> node_filter_ids_;
    std::map<std::string, int> node_filter_item_ids_;
    int next_filter_item_id_ = 2;
    std::map<int, std::string> level_filter_ids_;
    std::map<std::string, int> level_filter_item_ids_;
    RunLogTextView text_;
    juce::ComboBox node_filter_, level_filter_;
    juce::ComboBox history_;
    juce::TextButton history_refresh_{"History"};
    juce::Label history_status_;
    std::vector<RunLogArchiveReference> archives_;
    std::function<void()> refresh_history_;
    std::function<void(std::optional<RunLogArchiveReference>)> select_history_;
    juce::TextButton copy_{"Copy logs"};
};
}
