#pragma once
#include "EditorUiSupport.h"
#include "audio_run_api/NodeLogEvent.h"
#include "audio_core/Utf8LogDecoder.h"
#include <deque>
#include <map>
#include <optional>
#include <string>

namespace audioalgo::editor {
class RunLogPanel final : public juce::Component {
public:
    explicit RunLogPanel(const juce::String& id = "project.run_logs", std::string heading = "Run") : heading_(std::move(heading)) {
        addAndMakeVisible(text_); addAndMakeVisible(copy_); addAndMakeVisible(node_filter_);
        text_.setComponentID(id); text_.setMultiLine(true, false); text_.setReadOnly(true);
        text_.setScrollbarsShown(true); text_.setReturnKeyStartsNewLine(true);
        copy_.setComponentID(id + ".copy"); style_button(copy_);
        node_filter_.setComponentID(id + ".node_filter");
        node_filter_.setColour(juce::ComboBox::backgroundColourId, panel);
        node_filter_.setColour(juce::ComboBox::textColourId, ink);
        node_filter_.setColour(juce::ComboBox::outlineColourId, muted);
        node_filter_.setColour(juce::ComboBox::arrowColourId, signal);
        node_filter_.onChange = [this] { change_node_filter(); };
        copy_.onClick = [this] { juce::SystemClipboard::copyTextToClipboard(text_.getHighlightedText().isEmpty() ? text_.getText() : text_.getHighlightedText()); };
        rebuild_filter_items();
    }
    void clear() {
        entries_.clear(); retained_bytes_ = 0; displayed_bytes_.clear(); displayed_context_.clear();
        selected_node_.reset(); text_.clear(); rebuild_filter_items();
    }
    void append(const NodeLogEvent& event) {
        auto header = "[" + heading_ + " " + event.identity.request_id + " project=" + event.identity.project_id +
            " session=" + std::to_string(event.identity.session_epoch) + " revision=" + std::to_string(event.identity.revision) +
            " node=" + event.node_id + " type=" + event.type_id + " version=" + event.version +
            " package=" + event.package_id + " hash=" + event.content_sha256 +
            " " + event.source + "/" + event.level + "]\n";
        if (!event.report_file.empty()) header += "Report: " + event.report_file + "\n";
        if (!event.raw_log_file.empty()) header += "Full stream: " + event.raw_log_file + "\n";

        std::string value = utf8_string(event.text).replace("\r\n", "\n").toStdString();
        const bool has_marker = event.discarded_bytes || event.dropped_text_bytes || event.ui_dropped_events || event.ui_dropped_text_bytes;
        if (has_marker && !value.empty() && value.back() != '\n') value += '\n';
        if (event.discarded_bytes) value += "[stream tail discarded " + std::to_string(event.discarded_bytes) + " bytes]\n";
        if (event.dropped_text_bytes) value += "[display stream dropped " + std::to_string(event.dropped_text_bytes) + " UTF-8 bytes; full stream archived]\n";
        if (event.ui_dropped_events || event.ui_dropped_text_bytes) value += "[UI queue dropped " + std::to_string(event.ui_dropped_events) + " events / " +
            std::to_string(event.ui_dropped_text_bytes) + " text bytes; full stream archived]\n";

        entries_.push_back({event.node_id, std::move(header), std::move(value)});
        retained_bytes_ += entries_.back().context.size() + entries_.back().text.size() + 1;
        if (!node_filter_item_ids_.contains(event.node_id)) add_node_filter_item(event.node_id);
        if (trim_tail()) {
            rebuild_filter_items();
            rebuild_display();
            return;
        }
        const auto& entry = entries_.back();
        if (!selected_node_ || *selected_node_ == entry.node_id)
            append_to_display(entry);
    }
    void resized() override {
        node_filter_.setBounds(0, 0, std::min(260, std::max(0, getWidth() - 100)), 24);
        copy_.setBounds(getWidth() - 90, 0, 86, 24);
        text_.setBounds(0, 28, getWidth(), std::max(0, getHeight() - 28));
    }

private:
    struct Entry { std::string node_id, context, text; };
    static constexpr std::size_t display_limit = 256 * 1024;
    static constexpr std::size_t trim_target = 192 * 1024;

    void rebuild_filter_items() {
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
            if (!selected_node_ || *selected_node_ == entry.node_id)
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
    std::map<int, std::string> node_filter_ids_;
    std::map<std::string, int> node_filter_item_ids_;
    int next_filter_item_id_ = 2;
    juce::TextEditor text_;
    juce::ComboBox node_filter_;
    juce::TextButton copy_{"Copy logs"};
};
}
