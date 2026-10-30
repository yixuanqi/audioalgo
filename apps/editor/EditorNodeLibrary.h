#pragma once
#include "EditorUiSupport.h"
#include "EditorNodeCreation.h"
#include "EditorNodeDragDrop.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <string_view>
namespace audioalgo::editor {
class NodeLibraryHeading final : public juce::Label {
public:
    using Toggle = std::function<void(std::string, std::string, bool, bool)>;

    NodeLibraryHeading(const juce::String& text, std::string component_id, std::string category_id,
                       std::string subcategory_id, bool category_heading, bool collapsed,
                       std::size_t entry_count, Toggle toggle)
        : category_id_(std::move(category_id)), subcategory_id_(std::move(subcategory_id)),
          category_heading_(category_heading), collapsed_(collapsed), entry_count_(entry_count),
          toggle_(std::move(toggle)) {
        setComponentID(utf8_string(component_id));
        setText(text, juce::dontSendNotification);
        setJustificationType(juce::Justification::centredLeft);
        setBorderSize(juce::BorderSize<int>(0, 17, 0, 34));
        setWantsKeyboardFocus(true);
        setInterceptsMouseClicks(true, false);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        update_description();
    }

    void mouseDown(const juce::MouseEvent& event) override {
        mouse_pressed_ = event.mods.isLeftButtonDown() && getLocalBounds().toFloat().contains(event.position);
    }

    void mouseUp(const juce::MouseEvent& event) override {
        const bool activate = mouse_pressed_ && getLocalBounds().toFloat().contains(event.position);
        mouse_pressed_ = false;
        if (activate)
            toggle();
    }

    void set_entry_count(std::size_t entry_count) {
        if (entry_count_ == entry_count)
            return;
        entry_count_ = entry_count;
        update_description();
        repaint();
    }

    void set_search_projection_expanded(bool expanded) {
        if (search_projection_expanded_ == expanded)
            return;
        search_projection_expanded_ = expanded;
        update_description();
        repaint();
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (key.isKeyCode(juce::KeyPress::returnKey) || key.isKeyCode(juce::KeyPress::spaceKey)) {
            toggle();
            return true;
        }
        return juce::Component::keyPressed(key);
    }

    void paint(juce::Graphics& graphics) override {
        juce::Label::paint(graphics);
        const auto center_y = static_cast<float>(getHeight()) * 0.5f;
        juce::Path disclosure;
        if (is_collapsed_for_view())
            disclosure.addTriangle(5.0f, center_y - 4.0f, 5.0f, center_y + 4.0f, 11.0f, center_y);
        else
            disclosure.addTriangle(4.0f, center_y - 3.0f, 12.0f, center_y - 3.0f, 8.0f, center_y + 3.0f);
        graphics.setColour(ink.withAlpha(0.82f));
        graphics.fillPath(disclosure);
        const auto count_bounds = getLocalBounds().reduced(2).withTrimmedLeft(std::max(0, getWidth() - 34));
        graphics.setColour(muted.withAlpha(0.14f));
        graphics.fillRoundedRectangle(count_bounds.toFloat().reduced(1.0f), 4.0f);
        graphics.setColour(muted);
        graphics.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        graphics.drawFittedText(entry_count_text(), count_bounds,
                                juce::Justification::centred, 1);
        if (hasKeyboardFocus(true)) {
            graphics.setColour(signal.withAlpha(0.9f));
            graphics.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 3.0f, 1.2f);
        }
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override {
        juce::AccessibilityActions actions;
        actions.addAction(juce::AccessibilityActionType::press, [this] { toggle(); });
        return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::button,
                                                            std::move(actions));
    }

private:
    [[nodiscard]] juce::String entry_count_text() const {
        return juce::String(static_cast<int>(entry_count_));
    }

    [[nodiscard]] juce::String entry_count_description() const {
        return entry_count_text() + (entry_count_ == 1 ? " entry" : " entries");
    }

    void toggle() {
        collapsed_ = !collapsed_;
        update_description();
        repaint();
        if (toggle_)
            toggle_(category_id_, subcategory_id_, !subcategory_id_.empty(), collapsed_);
    }

    void update_description() {
        const auto group = category_heading_ ? "category" : "subcategory";
        setDescription(juce::String(group) + "; " + entry_count_description() +
                       (is_collapsed_for_view() ? "; collapsed; press Enter or Space to expand" :
                                                  "; expanded; press Enter or Space to collapse"));
        setTooltip(getDescription());
    }

    [[nodiscard]] bool is_collapsed_for_view() const noexcept {
        return collapsed_ && !search_projection_expanded_;
    }

    std::string category_id_;
    std::string subcategory_id_;
    bool category_heading_ = false;
    bool collapsed_ = false;
    bool search_projection_expanded_ = false;
    std::size_t entry_count_ = 0;
    bool mouse_pressed_ = false;
    Toggle toggle_;
};

class NodeLibraryButton final : public juce::TextButton {
public:
    NodeLibraryButton(const juce::String& text, std::string type_id,
                      std::function<void(std::string)> click_callback,
                      std::function<std::uint64_t()> project_epoch)
        : juce::TextButton(text), type_id_(std::move(type_id)), click_callback_(std::move(click_callback)),
          project_epoch_(std::move(project_epoch)) {
        onClick = [this] {
            if (click_callback_)
                click_callback_(type_id_);
        };
    }

    void mouseDown(const juce::MouseEvent& event) override {
        gesture_.begin({event.position.x, event.position.y});
        gesture_project_epoch_ = project_epoch_ ? project_epoch_() : 0;
        juce::TextButton::mouseDown(event);
    }

    [[nodiscard]] std::uint64_t gesture_project_epoch() const noexcept { return gesture_project_epoch_; }

    void mouseDrag(const juce::MouseEvent& event) override {
        if (!gesture_.move({event.position.x, event.position.y})) {
            juce::TextButton::mouseDrag(event);
            return;
        }
        auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this);
        if (container == nullptr)
            return;
        setState(juce::Button::buttonNormal);
        container->startDragging(make_library_drag_description(type_id_), this, juce::ScaledImage{}, false,
                                 nullptr, &event.source);
    }

    void mouseUp(const juce::MouseEvent& event) override {
        const auto release = gesture_.finish(getLocalBounds().toFloat().contains(event.position));
        if (release == audioalgo::editor::LibraryNodeDragGesture::Release::drag) {
            setState(juce::Button::buttonNormal);
        } else {
            juce::TextButton::mouseUp(event);
        }
    }

private:
    std::string type_id_;
    std::function<void(std::string)> click_callback_;
    std::function<std::uint64_t()> project_epoch_;
    audioalgo::editor::LibraryNodeDragGesture gesture_;
    std::uint64_t gesture_project_epoch_ = 0;
};

class NodeLibrary final : public juce::Component {
public:
    NodeLibrary(const NodeCatalog& catalog, std::function<void(std::string)> clicked,
                std::function<std::uint64_t()> epoch,
                std::function<void()> create_custom = {}, std::function<void()> open_draft = {}, std::function<void()> upgrade = {}, std::function<void()> recover = {},
                std::function<void()> open_workspace = {},
                std::function<void()> focus_search_requested = {})
        : clicked_(std::move(clicked)), epoch_(std::move(epoch)),
          focus_search_requested_(std::move(focus_search_requested)) {
        setInterceptsMouseClicks(false, true);
        addAndMakeVisible(title_);
        addAndMakeVisible(search_);
        addAndMakeVisible(source_filter_label_);
        addAndMakeVisible(source_filter_);
        addAndMakeVisible(language_filter_label_);
        addAndMakeVisible(language_filter_);
        addAndMakeVisible(clear_search_);
        addAndMakeVisible(no_results_);
        addAndMakeVisible(viewport_);
        addAndMakeVisible(new_custom_); addAndMakeVisible(open_draft_);
        addAndMakeVisible(open_workspace_); open_workspace_.setComponentID("authoring.open_workspace");
        open_workspace_.onClick = std::move(open_workspace);
        open_workspace_.setEnabled(static_cast<bool>(open_workspace_.onClick)); style_button(open_workspace_);
        open_workspace_.setTooltip("Open a node project folder; the directory is authoritative");
        addAndMakeVisible(recover_); recover_.setComponentID("authoring.recover"); recover_.onClick = std::move(recover);
        recover_.setEnabled(static_cast<bool>(recover_.onClick)); style_button(recover_);
        addAndMakeVisible(upgrade_); upgrade_.setComponentID("authoring.upgrade"); upgrade_.onClick = std::move(upgrade);
        upgrade_.setEnabled(static_cast<bool>(upgrade_.onClick)); style_button(upgrade_);
        new_custom_.setComponentID("authoring.new"); open_draft_.setComponentID("authoring.open");
        new_custom_.onClick = std::move(create_custom); open_draft_.onClick = std::move(open_draft);
        new_custom_.setEnabled(static_cast<bool>(new_custom_.onClick));
        open_draft_.setEnabled(static_cast<bool>(open_draft_.onClick));
        style_button(new_custom_, signal); style_button(open_draft_);
        title_.setText("NODE LIBRARY", juce::dontSendNotification);
        title_.setColour(juce::Label::textColourId, muted);
        title_.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        search_.setComponentID("library.search");
        search_.setMultiLine(false);
        search_.setReturnKeyStartsNewLine(false);
        search_.setTextToShowWhenEmpty("Search nodes…", muted);
        search_.setDescription("Search node name, type ID, description, tags, and category; terms use AND matching.");
        search_.setTooltip(search_.getDescription());
        search_.onTextChange = [this] { update_search_projection(); };
        source_filter_label_.setText("Source", juce::dontSendNotification);
        source_filter_label_.setColour(juce::Label::textColourId, muted);
        source_filter_label_.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        source_filter_.setComponentID("library.filter.source");
        source_filter_.addItem("All", 1);
        source_filter_.addItem("Built-in", 2);
        source_filter_.addItem("Custom", 3);
        source_filter_.addItem("Third party", 4);
        source_filter_.setSelectedId(1, juce::dontSendNotification);
        source_filter_.setDescription("Filter nodes by trusted package source.");
        source_filter_.setTooltip(source_filter_.getDescription());
        source_filter_.onChange = [this] { update_search_projection(); };
        language_filter_label_.setText("Language", juce::dontSendNotification);
        language_filter_label_.setColour(juce::Label::textColourId, muted);
        language_filter_label_.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        language_filter_.setComponentID("library.filter.language");
        language_filter_.addItem("All", 1);
        language_filter_.addItem("C++", 2);
        language_filter_.addItem("Python", 3);
        language_filter_.setSelectedId(1, juce::dontSendNotification);
        language_filter_.setDescription("Filter nodes by trusted implementation language.");
        language_filter_.setTooltip(language_filter_.getDescription());
        language_filter_.onChange = [this] { update_search_projection(); };
        clear_search_.setComponentID("library.search.clear");
        clear_search_.setButtonText("Clear");
        clear_search_.setTooltip("Clear node search and source/language filters");
        clear_search_.setDescription("Clear all node filters, restore the previous category folding, and return focus to node search.");
        clear_search_.onClick = [this] {
            clear_all_filters();
            search_.grabKeyboardFocus();
            if (focus_search_requested_)
                focus_search_requested_();
        };
        clear_search_.setEnabled(false);
        style_button(clear_search_);
        no_results_.setComponentID("library.search.no_results");
        no_results_.setText("No nodes match this search.", juce::dontSendNotification);
        no_results_.setDescription("No nodes match this search or filter combination. Clear filters to show the full node library.");
        no_results_.setTooltip(no_results_.getDescription());
        no_results_.setJustificationType(juce::Justification::centredLeft);
        no_results_.setColour(juce::Label::textColourId, muted);
        no_results_.setVisible(false);
        viewport_.setScrollBarsShown(true, false);
        refresh(catalog);
    }
    struct LayoutRow {
        enum class Kind { category_heading, subcategory_heading, node_entry };
        juce::Component* component = nullptr;
        int left_inset = 0;
        int height = 0;
        int gap_after = 0;
        Kind kind = Kind::node_entry;
        std::string category_id;
        std::string subcategory_id;
        std::vector<std::string> search_fields;
        std::string source_id;
        std::string language_id;
    };
    struct PreparedRefresh {
        std::unique_ptr<juce::Component> content;
        std::vector<std::unique_ptr<NodeLibraryButton>> buttons;
        std::vector<std::unique_ptr<juce::Label>> headings;
        std::vector<LayoutRow> rows;
    };
    [[nodiscard]] static std::string search_group_key(const LayoutRow& row) {
        if (row.kind == LayoutRow::Kind::subcategory_heading ||
            (row.kind == LayoutRow::Kind::node_entry && !row.subcategory_id.empty()))
            return "subcategory:" + row.category_id + "." + row.subcategory_id;
        return "category:" + row.category_id;
    }

    [[nodiscard]] bool matches_node_filters(const LayoutRow& row) const {
        if (row.kind != LayoutRow::Kind::node_entry ||
            !node_library_search_matches(search_tokens_, row.search_fields))
            return false;
        const auto source_id = source_filter_value(source_filter_.getSelectedId());
        const auto language_id = language_filter_value(language_filter_.getSelectedId());
        return (source_id.empty() || row.source_id == source_id) &&
               (language_id.empty() || row.language_id == language_id);
    }

    [[nodiscard]] bool has_search_query() const noexcept { return !search_tokens_.empty(); }

    [[nodiscard]] bool has_source_language_filter() const noexcept {
        return source_filter_.getSelectedId() != 1 || language_filter_.getSelectedId() != 1;
    }

    [[nodiscard]] std::size_t matching_entry_count(const LayoutRow& group) const {
        return static_cast<std::size_t>(std::count_if(rows_.begin(), rows_.end(), [&](const LayoutRow& candidate) {
            return matches_node_filters(candidate) && candidate.category_id == group.category_id &&
                   (group.kind != LayoutRow::Kind::subcategory_heading ||
                    candidate.subcategory_id == group.subcategory_id);
        }));
    }

    PreparedRefresh prepare_refresh(const NodeCatalog& catalog) {
        PreparedRefresh replacement{std::make_unique<juce::Component>(), {}, {}, {}};
        replacement.content->setComponentID("library.content");
        const auto entries = build_node_library_entries(catalog);
        const auto taxonomy = node_browser_taxonomy();
        if (taxonomy.empty())
            return replacement;

        struct CategoryEntries {
            std::vector<const NodeLibraryEntry*> direct;
            std::vector<std::vector<const NodeLibraryEntry*>> subcategories;
        };
        std::vector<CategoryEntries> grouped(taxonomy.size());
        for (std::size_t index = 0; index < taxonomy.size(); ++index)
            grouped[index].subcategories.resize(taxonomy[index].subcategory_ids.size());

        const auto uncategorized = taxonomy.size() - 1;
        for (const auto& entry : entries) {
            auto category = std::find_if(taxonomy.begin(), taxonomy.end(), [&](const auto& definition) {
                return definition.id == entry.browser_classification.category_id;
            });
            const auto category_index = category == taxonomy.end()
                ? uncategorized : static_cast<std::size_t>(std::distance(taxonomy.begin(), category));
            const auto& subcategory_ids = taxonomy[category_index].subcategory_ids;
            if (entry.browser_classification.subcategory_id) {
                const auto subcategory = std::find(subcategory_ids.begin(), subcategory_ids.end(),
                                                   *entry.browser_classification.subcategory_id);
                if (subcategory != subcategory_ids.end()) {
                    const auto subcategory_index = static_cast<std::size_t>(
                        std::distance(subcategory_ids.begin(), subcategory));
                    grouped[category_index].subcategories[subcategory_index].push_back(&entry);
                    continue;
                }
            }
            grouped[category_index].direct.push_back(&entry);
        }

        const auto byte_less = [](std::string_view left, std::string_view right) {
            return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                [](char lhs, char rhs) {
                    return static_cast<unsigned char>(lhs) < static_cast<unsigned char>(rhs);
                });
        };
        struct SortKey {
            const NodeLibraryEntry* entry = nullptr;
            std::string folded_display_name;
        };
        const auto folded_name = [](const NodeLibraryEntry& entry) {
            const auto normalized = utf8_string(entry.display_name).toLowerCase();
            const auto encoded = normalized.toUTF8();
            return std::string(encoded.getAddress());
        };
        const auto append_heading = [&](const juce::String& text, std::string component_id,
                                       int inset, int height, int gap_after, bool category_heading,
                                       std::size_t entry_count,
                                       const std::string& category_id, const std::string& subcategory_id) {
            const auto key = category_id + "." + subcategory_id;
            const bool collapsed = category_heading ? collapsed_categories_.contains(category_id)
                                                    : collapsed_subcategories_.contains(key);
            auto heading = std::make_unique<NodeLibraryHeading>(text, component_id, category_id,
                subcategory_id, category_heading, collapsed, entry_count,
                [this](std::string category, std::string subcategory, bool is_subcategory, bool should_collapse) {
                    set_group_collapsed(category, subcategory, is_subcategory, should_collapse);
                });
            heading->setFont(juce::FontOptions(category_heading ? 12.5f : 11.0f,
                                                category_heading ? juce::Font::bold : juce::Font::plain));
            heading->setColour(juce::Label::textColourId, category_heading ? ink : muted);
            replacement.content->addAndMakeVisible(*heading);
            replacement.rows.push_back({heading.get(), inset, height, gap_after,
                category_heading ? LayoutRow::Kind::category_heading : LayoutRow::Kind::subcategory_heading,
                category_id, subcategory_id});
            replacement.headings.push_back(std::move(heading));
        };
        const auto append_entry = [&](const NodeLibraryEntry& entry, int inset,
                                      const std::string& category_id, const std::string& subcategory_id) {
            auto button = std::make_unique<NodeLibraryButton>(utf8_string(entry.display_name), entry.type_id,
                                                               clicked_, epoch_);
            button->setComponentID("library." + utf8_string(entry.type_id));
            if (const auto* descriptor = catalog.find(entry.type_id)) {
                const auto notice = native_user_log_notice(*descriptor);
                if (notice.isNotEmpty())
                    button->setTooltip(utf8_string(entry.display_name) + "\n" + notice);
            }
            style_button(*button);
            replacement.content->addAndMakeVisible(*button);
            std::vector<std::string> search_fields{
                node_library_search_fold_ascii(entry.display_name),
                node_library_search_fold_ascii(entry.type_id),
                node_library_search_fold_ascii(entry.browser_description)};
            const auto category = std::find_if(taxonomy.begin(), taxonomy.end(), [&](const auto& definition) {
                return definition.id == category_id;
            });
            if (category != taxonomy.end())
                search_fields.push_back(node_library_search_fold_ascii(category->display_name));
            for (const auto& tag : entry.browser_tags)
                search_fields.push_back(node_library_search_fold_ascii(tag));
            replacement.rows.push_back({button.get(), inset, 34, 8, LayoutRow::Kind::node_entry,
                                        category_id, subcategory_id, std::move(search_fields),
                                        entry.source_id, entry.language_id});
            replacement.buttons.push_back(std::move(button));
        };
        const auto append_sorted_entries = [&](std::vector<const NodeLibraryEntry*>& values, int inset,
                                               const std::string& category_id,
                                               const std::string& subcategory_id) {
            std::vector<SortKey> ordered;
            ordered.reserve(values.size());
            for (const auto* entry : values)
                ordered.push_back({entry, folded_name(*entry)});
            std::sort(ordered.begin(), ordered.end(), [&](const SortKey& left, const SortKey& right) {
                if (left.folded_display_name != right.folded_display_name)
                    return byte_less(left.folded_display_name, right.folded_display_name);
                return byte_less(left.entry->type_id, right.entry->type_id);
            });
            for (const auto& item : ordered)
                append_entry(*item.entry, inset, category_id, subcategory_id);
        };
        const auto subcategory_label = [](std::string_view id) {
            std::string result{id};
            bool capitalize = true;
            for (auto& character : result) {
                if (character == '-') {
                    character = ' ';
                    capitalize = true;
                } else if (capitalize) {
                    if (character >= 'a' && character <= 'z')
                        character = static_cast<char>(character - 'a' + 'A');
                    capitalize = false;
                }
            }
            return utf8_string(result);
        };
        const auto count_category_entries = [](const CategoryEntries& category_entries) {
            auto count = category_entries.direct.size();
            for (const auto& subcategory_entries : category_entries.subcategories)
                count += subcategory_entries.size();
            return count;
        };

        for (std::size_t category_index = 0; category_index < taxonomy.size(); ++category_index) {
            const auto& definition = taxonomy[category_index];
            auto& category_entries = grouped[category_index];
            const bool has_subcategory_entries = std::any_of(
                category_entries.subcategories.begin(), category_entries.subcategories.end(),
                [](const auto& values) { return !values.empty(); });
            if (category_entries.direct.empty() && !has_subcategory_entries)
                continue;

            append_heading(utf8_string(std::string(definition.display_name)),
                           "library.category." + std::string(definition.id), 2, 24, 4, true,
                           count_category_entries(category_entries),
                           std::string(definition.id), {});
            append_sorted_entries(category_entries.direct, 12, std::string(definition.id), {});
            for (std::size_t subcategory_index = 0;
                 subcategory_index < definition.subcategory_ids.size(); ++subcategory_index) {
                auto& values = category_entries.subcategories[subcategory_index];
                if (values.empty())
                    continue;
                const auto subcategory_id = definition.subcategory_ids[subcategory_index];
                append_heading(subcategory_label(subcategory_id),
                               "library.subcategory." + std::string(definition.id) + "." +
                                   std::string(subcategory_id), 12, 20, 2, false, values.size(),
                               std::string(definition.id), std::string(subcategory_id));
                append_sorted_entries(values, 22, std::string(definition.id), std::string(subcategory_id));
            }
        }
        return replacement;
    }
    void publish_refresh(PreparedRefresh replacement) {
        const auto scroll = viewport_.getViewPosition();
        viewport_.setViewedComponent(replacement.content.get(), false);
        content_.swap(replacement.content); buttons_.swap(replacement.buttons);
        headings_.swap(replacement.headings); rows_.swap(replacement.rows);
        update_search_projection();
        viewport_.setViewPosition(scroll);
    }
    void refresh(const NodeCatalog& catalog) { publish_refresh(prepare_refresh(catalog)); }
    void resized() override {
        title_.setBounds(18, 24, getWidth() - 34, 22);
        new_custom_.setBounds(12, 52, getWidth() - 24, 30);
        open_draft_.setBounds(12, 86, getWidth() - 24, 28);
        open_workspace_.setBounds(12, 118, getWidth() - 24, 28);
        recover_.setBounds(12, 150, getWidth() - 24, 28);
        search_.setBounds(12, 182, std::max(96, getWidth() - 92), 28);
        clear_search_.setBounds(std::max(12, getWidth() - 68), 182, 56, 28);
        const int filter_gap = 12;
        const int filter_width = std::max(64, (getWidth() - 24 - filter_gap) / 2);
        const int language_x = 12 + filter_width + filter_gap;
        source_filter_label_.setBounds(12, 214, filter_width, 14);
        source_filter_.setBounds(12, 230, filter_width, 28);
        language_filter_label_.setBounds(language_x, 214, filter_width, 14);
        language_filter_.setBounds(language_x, 230, filter_width, 28);
        no_results_.setBounds(12, 266, getWidth() - 24, 26);
        const int viewport_y = no_results_.isVisible() ? 300 : 266;
        viewport_.setBounds(10, viewport_y, getWidth() - 20,
                            std::max(32, getHeight() - (viewport_y + 56)));
        upgrade_.setBounds(12, getHeight() - 40, getWidth() - 24, 28);
        const int width = std::max(96, viewport_.getWidth() - 20);
        int y = 8;
        for (const auto& row : rows_) {
            const auto visible = is_row_visible(row);
            row.component->setVisible(visible);
            if (!visible) {
                row.component->setBounds(row.left_inset, y, std::max(0, width - row.left_inset), 0);
                continue;
            }
            row.component->setBounds(row.left_inset, y, std::max(0, width - row.left_inset), row.height);
            y += row.height + row.gap_after;
        }
        if (content_) content_->setSize(viewport_.getWidth() - 8, y);
    }
private:
    [[nodiscard]] static std::string_view source_filter_value(int selected_id) noexcept {
        switch (selected_id) {
        case 2: return "builtin";
        case 3: return "custom";
        case 4: return "third_party";
        default: return {};
        }
    }

    [[nodiscard]] static std::string_view language_filter_value(int selected_id) noexcept {
        switch (selected_id) {
        case 2: return "cpp";
        case 3: return "python";
        default: return {};
        }
    }

    [[nodiscard]] bool is_row_visible(const LayoutRow& row) const {
        const bool searching = has_search_query();
        const bool filtering = has_source_language_filter();
        if (row.kind == LayoutRow::Kind::category_heading) {
            if (searching)
                return search_visible_groups_.contains("category:" + row.category_id);
            return !filtering || matching_entry_count(row) > 0;
        }
        if (row.kind == LayoutRow::Kind::subcategory_heading) {
            if (searching)
                return search_visible_groups_.contains("subcategory:" + row.category_id + "." +
                                                       row.subcategory_id);
            if (collapsed_categories_.contains(row.category_id))
                return false;
            return !filtering || matching_entry_count(row) > 0;
        }
        if (!matches_node_filters(row))
            return false;
        if (searching)
            return true;
        if (collapsed_categories_.contains(row.category_id))
            return false;
        return row.subcategory_id.empty() ||
               !collapsed_subcategories_.contains(row.category_id + "." + row.subcategory_id);
    }

    void update_search_projection() {
        const auto query_utf8 = search_.getText().toStdString();
        search_tokens_ = node_library_search_tokens(query_utf8);
        clear_search_.setEnabled(!search_tokens_.empty() || has_source_language_filter());
        search_visible_groups_.clear();
        const bool searching = has_search_query();
        const bool filtering = has_source_language_filter();
        for (const auto& heading_row : rows_) {
            if (heading_row.kind == LayoutRow::Kind::node_entry)
                continue;
            const auto count = matching_entry_count(heading_row);
            const auto group_key = search_group_key(heading_row);
            if (count > 0)
                search_visible_groups_.insert(group_key);
            if (auto* heading = dynamic_cast<NodeLibraryHeading*>(heading_row.component)) {
                heading->set_entry_count(static_cast<std::size_t>(count));
                heading->set_search_projection_expanded(searching);
            }
        }
        const auto has_match = std::any_of(rows_.begin(), rows_.end(),
                                           [&](const LayoutRow& row) { return matches_node_filters(row); });
        no_results_.setText(searching ? "No nodes match this search." : "No nodes match these filters.",
                            juce::dontSendNotification);
        no_results_.setVisible((searching || filtering) && !has_match);
        resized();
    }

    void clear_all_filters() {
        source_filter_.setSelectedId(1, juce::dontSendNotification);
        language_filter_.setSelectedId(1, juce::dontSendNotification);
        search_.setText({}, false);
        update_search_projection();
    }

    void set_group_collapsed(const std::string& category_id, const std::string& subcategory_id,
                             bool subcategory, bool collapsed) {
        if (subcategory) {
            const auto key = category_id + "." + subcategory_id;
            if (collapsed)
                collapsed_subcategories_.insert(key);
            else
                collapsed_subcategories_.erase(key);
        } else if (collapsed) {
            collapsed_categories_.insert(category_id);
        } else {
            collapsed_categories_.erase(category_id);
        }
        resized();
    }

    std::function<void(std::string)> clicked_;
    std::function<std::uint64_t()> epoch_;
    std::function<void()> focus_search_requested_;
    juce::TextButton new_custom_{"New custom node"}, open_draft_{"Open draft"};
    juce::TextButton open_workspace_{"Open node project"};
    juce::TextButton upgrade_{"Upgrade project"};
    juce::TextButton recover_{"Recover draft"};
    juce::TextButton clear_search_{"Clear"};
    juce::Label title_;
    juce::Label source_filter_label_;
    juce::ComboBox source_filter_;
    juce::Label language_filter_label_;
    juce::ComboBox language_filter_;
    juce::Label no_results_;
    juce::TextEditor search_;
    std::unique_ptr<juce::Component> content_;
    juce::Viewport viewport_;
    std::vector<std::unique_ptr<NodeLibraryButton>> buttons_;
    std::vector<std::unique_ptr<juce::Label>> headings_;
    std::vector<LayoutRow> rows_;
    std::vector<std::string> search_tokens_;
    std::set<std::string> collapsed_categories_;
    std::set<std::string> collapsed_subcategories_;
    std::set<std::string> search_visible_groups_;
};

} // namespace audioalgo::editor
