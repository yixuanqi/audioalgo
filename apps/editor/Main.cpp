#include "EditorGeometry.h"
#include "EditorComponentLayout.h"
#include "EditorText.h"
#include "EditorNodeCreation.h"
#include "EditorNodeDragDrop.h"
#include "EditorTopologyHistory.h"
#include "EditorTopologyCommands.h"
#include "EditorKeyboardShortcuts.h"
#include "audio_node_sdk/NodeSDK.h"
#include "audio_io/WavFile.h"
#include "audio_project/ProjectFileIO.h"
#include "audio_project/ProjectModel.h"
#include "audio_run_service/RunService.h"

#include <juce_audio_utils/juce_audio_utils.h>
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

namespace {

using audioalgo::editor::to_utf8;
using audioalgo::editor::utf8_literal;
using audioalgo::editor::utf8_string;

const auto background = juce::Colour(0xff101b23);
const auto panel = juce::Colour(0xff1f2d36);
const auto raised = juce::Colour(0xff263842);
const auto ink = juce::Colour(0xffe6ece7);
const auto muted = juce::Colour(0xff9aabb0);
const auto signal = juce::Colour(0xff9ad3c8);
const auto compatible_green = juce::Colour(0xff6ed39a);
const auto amber = juce::Colour(0xffe5b56f);
const auto danger = juce::Colour(0xffe28778);

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

juce::File to_juce_file(const std::filesystem::path& path) {
    const auto value = path_to_utf8(path);
    return juce::File(utf8_string(value));
}

std::filesystem::path path_from_juce_file(const juce::File& file) {
    return std::filesystem::u8path(to_utf8(file.getFullPathName()));
}

std::string new_id() {
    return juce::Uuid().toString().toStdString();
}

juce::var make_library_drag_description(const std::string& type_id) {
    auto* payload = new juce::DynamicObject();
    payload->setProperty("operation", "node-library-add");
    payload->setProperty("version", 1);
    payload->setProperty("type_id", utf8_string(type_id));
    return juce::var(payload);
}

std::optional<std::string> library_drag_type(const juce::var& description) {
    const auto* payload = description.getDynamicObject();
    if (payload == nullptr || payload->getProperty("operation").toString() != "node-library-add" ||
        static_cast<int>(payload->getProperty("version")) != 1)
        return std::nullopt;
    const auto type_id = to_utf8(payload->getProperty("type_id").toString());
    if (type_id.empty())
        return std::nullopt;
    return type_id;
}

const audioalgo::NodeInstance* find_node(const audioalgo::ProjectModel& project, const std::string& id) {
    const auto found = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) { return node.id == id; });
    return found == project.nodes.end() ? nullptr : &*found;
}

struct WaveformRange {
    float minimum = 0.0f;
    float maximum = 0.0f;
};
using WaveformChannels = std::vector<std::vector<WaveformRange>>;

WaveformChannels make_waveform(const audioalgo::AudioBuffer& audio, std::size_t columns = 1200) {
    WaveformChannels result(audio.channel_count());
    if (audio.frame_count() == 0 || columns == 0)
        return result;
    const auto count = std::min(columns, audio.frame_count());
    for (std::size_t channel = 0; channel < audio.channel_count(); ++channel) {
        auto& ranges = result[channel];
        ranges.reserve(count);
        const auto samples = audio.channel(channel);
        for (std::size_t column = 0; column < count; ++column) {
            const auto first = column * audio.frame_count() / count;
            const auto last = (column + 1) * audio.frame_count() / count;
            auto minimum = samples[first];
            auto maximum = samples[first];
            for (auto frame = first + 1; frame < last; ++frame) {
                minimum = std::min(minimum, samples[frame]);
                maximum = std::max(maximum, samples[frame]);
            }
            ranges.push_back({minimum, maximum});
        }
    }
    return result;
}

class WaveformView final : public juce::Component {
public:
    void set_waveform(WaveformChannels waveform) {
        waveform_ = std::move(waveform);
        repaint();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(background);
        g.setColour(panel);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
        g.setColour(muted);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(utf8_literal("Rendered waveform · peak-preserving overview"), 14, 8, getWidth() - 28, 20, juce::Justification::centredLeft);
        if (waveform_.empty() || waveform_.front().empty()) {
            g.setColour(muted);
            g.drawText("Run a project to inspect its audio output", 14, 32, getWidth() - 28, getHeight() - 40,
                       juce::Justification::centred);
            return;
        }

        const int lane_top = 34;
        const int lane_height = std::max(1, (getHeight() - lane_top - 10) / static_cast<int>(waveform_.size()));
        for (std::size_t channel = 0; channel < waveform_.size(); ++channel) {
            const auto y_mid = static_cast<float>(lane_top + static_cast<int>(channel) * lane_height + lane_height / 2);
            g.setColour(raised);
            g.drawHorizontalLine(static_cast<int>(y_mid), 12.0f, static_cast<float>(getWidth() - 12));
            g.setColour(signal.withAlpha(channel == 0 ? 0.95f : 0.62f));
            const auto& ranges = waveform_[channel];
            for (std::size_t column = 0; column < ranges.size(); ++column) {
                const auto x = 14.0f + static_cast<float>(column) * static_cast<float>(getWidth() - 28) /
                                                   static_cast<float>(std::max<std::size_t>(1, ranges.size() - 1));
                const auto scale = static_cast<float>(lane_height) * 0.43f;
                g.drawVerticalLine(static_cast<int>(x), y_mid - ranges[column].maximum * scale,
                                   y_mid - ranges[column].minimum * scale);
            }
            g.setColour(muted);
            g.setFont(juce::FontOptions(10.0f));
            g.drawText("CH " + juce::String(static_cast<int>(channel + 1)), 14,
                       lane_top + static_cast<int>(channel) * lane_height, 42, 14, juce::Justification::centredLeft);
        }
    }

private:
    WaveformChannels waveform_;
};

class GraphCanvas final : public juce::Component, public juce::SettableTooltipClient,
                          public juce::DragAndDropTarget {
public:
    GraphCanvas(audioalgo::ProjectModel& project, audioalgo::NodeRegistry& descriptors,
                audioalgo::editor::NodeSelectionState& selection,
                audioalgo::editor::ProjectDirtyState& dirty_state,
                audioalgo::editor::EditorTopologyHistory& history,
                std::function<void(std::string)> selection_changed,
                std::function<void()> layout_changed,
                std::function<void(std::string)> graph_error,
                std::function<void(std::string, audioalgo::editor::EditorPoint, std::uint64_t, std::uint64_t,
                                   std::optional<audioalgo::Connection>)> library_drop,
                std::function<std::optional<std::uint64_t>(juce::Component*)> library_drag_epoch)
        : project_(project), descriptors_(descriptors), selection_(selection), dirty_state_(dirty_state), history_(history),
          selection_changed_(std::move(selection_changed)),
          layout_changed_(std::move(layout_changed)), graph_error_(std::move(graph_error)),
          library_drop_(std::move(library_drop)), library_drag_epoch_(std::move(library_drag_epoch)) {
        setMouseCursor(juce::MouseCursor::NormalCursor);
        setWantsKeyboardFocus(true);
    }

    void select(std::string node_id) {
        const bool changed = selection_.select(project_, node_id);
        grabKeyboardFocus();
        if (!changed)
            return;
        selection_changed_(selection_.selected_node_id());
        repaint();
    }

    const std::string& selected_node_id() const noexcept { return selection_.selected_node_id(); }
    const audioalgo::editor::CanvasCoordinateTransform& coordinate_transform() const noexcept { return coordinates_; }

    void set_project_epoch(std::uint64_t epoch) noexcept {
        project_epoch_ = epoch;
        library_drop_session_.cancel();
        dragging_node_.clear();
        node_drag_preview_position_.reset();
        existing_node_insertion_target_.reset();
        node_order_swap_target_.reset();
        node_order_drop_target_.reset();
        connection_gesture_.cancel();
        panning_ = false;
        coordinates_.reset_pan();
        setMouseCursor(juce::MouseCursor::NormalCursor);
        selected_connection_index_.reset();
        repaint();
    }

    void finish_library_drag(std::uint64_t source_epoch) noexcept {
        library_drop_session_.finish_drag(source_epoch);
        repaint();
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(background);
        g.setColour(juce::Colour(0xff18252d));
        const auto pan = coordinates_.pan_offset();
        const auto grid_phase = [](float offset) {
            auto phase = std::fmod(offset, 28.0f);
            if (phase < 0.0f)
                phase += 28.0f;
            return static_cast<int>(phase) - 28;
        };
        for (int x = grid_phase(pan.x); x < getWidth(); x += 28)
            for (int y = grid_phase(pan.y); y < getHeight(); y += 28)
                g.fillEllipse(static_cast<float>(x), static_cast<float>(y), 1.5f, 1.5f);

        g.setColour(muted);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("SIGNAL FLOW", 18, 12, 120, 18, juce::Justification::centredLeft);
        const auto compatible_target = compatible_connection_target();
        paint_connections(g);
        if (connection_gesture_.is_active()) {
            const auto* source = find_node(project_, connection_gesture_.source_node_id());
            const auto start = source == nullptr ? std::optional<juce::Point<float>>{} :
                port_anchor(*source, connection_gesture_.source_port_id());
            if (start) {
                juce::Path path;
                path.startNewSubPath(*start);
                const auto preview_pointer = to_juce_point(connection_gesture_.pointer_position());
                path.cubicTo(start->translated(64.0f, 0.0f), preview_pointer.translated(-64.0f, 0.0f), preview_pointer);
                g.setColour(signal.withAlpha(0.75f));
                g.strokePath(path, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }
        for (const auto& node : project_.nodes)
            if (node.id == dragging_node_ && node_drag_preview_position_) {
                auto preview_node = node;
                preview_node.x = node_drag_preview_position_->x;
                preview_node.y = node_drag_preview_position_->y;
                paint_node(g, preview_node, compatible_target);
            } else {
                paint_node(g, node, compatible_target);
            }
        paint_library_drop_preview(g);
    }

    bool isInterestedInDragSource(const SourceDetails& details) override {
        const auto type_id = library_drag_type(details.description);
        const auto source_epoch = library_drag_epoch_ ? library_drag_epoch_(details.sourceComponent.get()) : std::nullopt;
        if (!type_id || !source_epoch || *source_epoch != project_epoch_)
            return false;
        const auto* descriptor = descriptors_.find(*type_id);
        return descriptor != nullptr && audioalgo::editor::can_add_node_of_execution_kind(
            project_, descriptors_, descriptor->execution_kind);
    }

    void itemDragEnter(const SourceDetails& details) override {
        const auto type_id = library_drag_type(details.description);
        const auto source_epoch = library_drag_epoch_ ? library_drag_epoch_(details.sourceComponent.get()) : std::nullopt;
        if (!type_id || !source_epoch || *source_epoch != project_epoch_ ||
            !library_drop_session_.begin(*type_id, *source_epoch, project_.revision)) {
            library_drop_session_.cancel();
            repaint();
            return;
        }
        update_library_drop_preview(details);
    }
    void itemDragMove(const SourceDetails& details) override {
        if (!library_drop_session_.is_active())
            itemDragEnter(details);
        else
            update_library_drop_preview(details);
    }
    void itemDragExit(const SourceDetails&) override {
        library_drop_session_.leave_canvas();
        repaint();
    }

    void itemDropped(const SourceDetails& details) override {
        const auto type_id = library_drag_type(details.description);
        const auto source_epoch = library_drag_epoch_ ? library_drag_epoch_(details.sourceComponent.get()) : std::nullopt;
        // JUCE does not guarantee a final itemDragMove before itemDropped. Re-evaluate
        // the release point so an edge hit cannot use stale preview state.
        if (type_id && source_epoch && *source_epoch == project_epoch_ && library_drop_session_.is_active())
            update_library_drop_preview(details);
        const auto drop_revision = library_drop_session_.project_revision();
        const bool may_deliver = type_id && source_epoch && *type_id == library_drop_session_.type_id() &&
            *source_epoch == library_drop_session_.source_epoch() &&
            library_drop_session_.can_deliver_drop(project_epoch_, project_.revision);
        const auto point = coordinates_.event_to_canvas({static_cast<float>(details.localPosition.x),
                                                         static_cast<float>(details.localPosition.y)});
        const auto target_connection = may_deliver ? library_drop_session_.target_connection() :
            std::optional<audioalgo::Connection>{};
        const auto accepted_type = may_deliver ? *type_id : std::string{};
        library_drop_session_.cancel();
        repaint();
        if (!accepted_type.empty() && library_drop_)
            library_drop_(accepted_type, point, *source_epoch, drop_revision, target_connection);
        else if (type_id && source_epoch &&
                 (*source_epoch != project_epoch_ || drop_revision != project_.revision) && graph_error_)
            graph_error_("The project changed during this drag; the node was not added.");
    }

    void mouseMove(const juce::MouseEvent& event) override {
        const auto canvas_point = coordinates_.event_to_canvas({event.position.x, event.position.y});
        mouse_position_ = to_juce_point(canvas_point);
        update_tooltip(mouse_position_);
        if (connection_gesture_.is_active()) {
            (void)connection_gesture_.update_pointer(canvas_point);
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& event) override {
        if (event.mods.isMiddleButtonDown()) {
            panning_ = true;
            last_pan_position_ = event.position;
            connection_gesture_.cancel();
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            return;
        }
        grabKeyboardFocus();
        const auto canvas_point = coordinates_.event_to_canvas({event.position.x, event.position.y});
        const auto canvas_position = to_juce_point(canvas_point);
        mouse_position_ = canvas_position;
        const auto hit_port = audioalgo::editor::find_topmost_port_at(
            project_.nodes, descriptors_, canvas_point, coordinates_);
        if (hit_port) {
            selected_connection_index_.reset();
            const auto* owner = find_node(project_, hit_port->node_id);
            if (owner == nullptr)
                return;
            if (hit_port->direction == audioalgo::PortDirection::output) {
                (void)connection_gesture_.begin(owner->id, hit_port->port_id, canvas_point);
                grabKeyboardFocus();
                dragging_node_.clear();
                repaint();
                return;
            }
            if (connection_gesture_.is_active())
                connect_to(*owner, hit_port->port_id);
            else if (graph_error_)
                graph_error_("Select an output socket first, then its input socket.");
            return;
        }

        const auto* hit_node = audioalgo::editor::find_topmost_node_at(
            project_.nodes, descriptors_, canvas_point, coordinates_);
        if (hit_node != nullptr) {
            selected_connection_index_.reset();
            const auto& node = *hit_node;
            const auto layout = node_geometry(node);
            if (to_juce_rect(layout.bounds).contains(canvas_position)) {
                (void)selection_.select(project_, node.id);
                selection_changed_(selection_.selected_node_id());
                dragging_node_ = node.id;
                const auto node_position = coordinates_.model_to_canvas(
                    {static_cast<float>(node.x), static_cast<float>(node.y)});
                drag_offset_ = canvas_position - to_juce_point(node_position);
                node_drag_preview_position_.reset();
                existing_node_insertion_target_.reset();
                node_order_swap_target_.reset();
                node_order_drop_target_.reset();
                repaint();
                return;
            }
        }
        const auto hit_connection = audioalgo::editor::find_visible_connection_at(
            project_, descriptors_, canvas_point, 8.0f, coordinates_);
        if (hit_connection) {
            selected_connection_index_ = *hit_connection;
            connection_gesture_.cancel();
            (void)selection_.select(project_, {});
            selection_changed_({});
            repaint();
            return;
        }
        selected_connection_index_.reset();
        if (connection_gesture_.is_active()) {
            connection_gesture_.cancel();
            repaint();
        }
        (void)selection_.select(project_, {});
        dragging_node_.clear();
        selection_changed_({});
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        if (panning_) {
            const juce::Point<float> delta = event.position - last_pan_position_;
            coordinates_.pan_by({delta.x, delta.y});
            last_pan_position_ = event.position;
            mouse_position_ = event.position;
            update_tooltip(mouse_position_);
            repaint();
            return;
        }
        if (connection_gesture_.is_active()) {
            const auto canvas_point = coordinates_.event_to_canvas({event.position.x, event.position.y});
            mouse_position_ = to_juce_point(canvas_point);
            (void)connection_gesture_.update_pointer(canvas_point);
            connection_gesture_.mark_drag_started();
            repaint();
            return;
        }
        if (dragging_node_.empty())
            return;
        const auto canvas_point = coordinates_.event_to_canvas({event.position.x, event.position.y});
        update_node_drag_preview(canvas_point);
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (panning_) {
            panning_ = false;
            setMouseCursor(juce::MouseCursor::NormalCursor);
            return;
        }
        if (connection_gesture_.is_dragging()) {
            const auto canvas_point = coordinates_.event_to_canvas({event.position.x, event.position.y});
            const auto hit_port = audioalgo::editor::find_topmost_port_at(
                project_.nodes, descriptors_, canvas_point, coordinates_);
            const auto* target = hit_port && hit_port->direction == audioalgo::PortDirection::input
                ? find_node(project_, hit_port->node_id) : nullptr;
            if (target != nullptr)
                connect_to(*target, hit_port->port_id);
            else {
                connection_gesture_.cancel();
                repaint();
            }
        }
        if (!dragging_node_.empty() && node_drag_preview_position_) {
            const auto canvas_point = coordinates_.event_to_canvas({event.position.x, event.position.y});
            update_node_drag_preview(canvas_point);
            const auto position = *node_drag_preview_position_;
            if (existing_node_insertion_target_) {
                const auto node_id = dragging_node_;
                const auto target_connection = *existing_node_insertion_target_;
                const auto* node = find_node(project_, node_id);
                const auto* descriptor = node == nullptr ? nullptr : descriptors_.find(node->type_id);
                const bool connected = std::any_of(project_.connections.begin(), project_.connections.end(),
                    [&](const auto& edge) { return edge.from_node == node_id || edge.to_node == node_id; });
                if (descriptor == nullptr || descriptor->execution_kind != audioalgo::NodeExecutionKind::block || connected) {
                    if (graph_error_)
                        graph_error_("This node cannot be inserted on that connection.");
                } else {
                    const auto choices = audioalgo::editor::find_library_insertion_port_choices(
                        project_, descriptors_, descriptor->type_id, target_connection);
                    if (choices.empty()) {
                        if (graph_error_)
                            graph_error_("This node has no compatible ports for that connection.");
                    } else if (choices.size() == 1) {
                        commit_existing_node_insertion(node_id, target_connection, position, choices.front());
                    } else {
                        show_existing_node_port_choice(node_id, target_connection, position, choices);
                    }
                }
            } else if (node_order_swap_target_) {
                const auto dragged_node_id = dragging_node_;
                const auto target_node_id = *node_order_swap_target_;
                if (history_.apply(project_, [&](auto& candidate) {
                        return audioalgo::editor::try_swap_adjacent_processing_nodes(
                            candidate, descriptors_, dragged_node_id, target_node_id);
                    })) {
                    (void)selection_.select(project_, dragged_node_id);
                    selection_changed_(selection_.selected_node_id());
                    layout_changed_();
                }
            } else if (node_order_drop_target_) {
                if (graph_error_)
                    graph_error_("Only adjacent processing nodes with a compatible, unbranched path can swap order.");
            } else if (audioalgo::editor::move_node_layout(project_, dirty_state_, dragging_node_,
                                                          position.x, position.y)) {
                layout_changed_();
            }
        }
        dragging_node_.clear();
        node_drag_preview_position_.reset();
        existing_node_insertion_target_.reset();
        node_order_swap_target_.reset();
        node_order_drop_target_.reset();
        repaint();
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (connection_gesture_.cancel_on_escape(key.getKeyCode() == juce::KeyPress::escapeKey)) {
            repaint();
            return true;
        }
        const auto key_code = key.getKeyCode();
        if ((key_code == juce::KeyPress::deleteKey || key_code == juce::KeyPress::backspaceKey) &&
            (!selection_.selected_node_id().empty() || selected_connection_index_)) {
            delete_selected();
            return true;
        }
        return false;
    }

    void delete_selected() {
        if (selected_connection_index_) {
            const auto connection_index = *selected_connection_index_;
            selected_connection_index_.reset();
            if (connection_index >= project_.connections.size()) {
                repaint();
                return;
            }
            const auto selected_connection = project_.connections[connection_index];
            if (history_.apply(project_, [&](auto& candidate) {
                    return audioalgo::editor::try_disconnect_connection(candidate, selected_connection);
                })) {
                layout_changed_();
                repaint();
            }
            return;
        }
        if (selection_.selected_node_id().empty())
            return;
        const auto removed_id = selection_.selected_node_id();
        if (!history_.apply(project_, [&](auto& candidate) {
                return audioalgo::editor::delete_node(candidate, descriptors_, removed_id);
            }))
            return;
        selected_connection_index_.reset();
        (void)selection_.select(project_, {});
        selection_changed_({});
        layout_changed_();
        repaint();
    }

private:
    void commit_existing_node_insertion(const std::string& node_id, const audioalgo::Connection& target,
                                        audioalgo::editor::EditorPoint position,
                                        const audioalgo::editor::InsertionPortChoice& choice) {
        std::optional<audioalgo::editor::LibraryNodeInsertionResult> inserted;
        const auto committed = history_.apply(project_, [&](auto& candidate) {
            inserted = audioalgo::editor::try_insert_existing_node_on_connection(
                candidate, descriptors_, node_id, target, position.x, position.y, choice);
            return inserted.has_value();
        });
        if (committed)
            layout_changed_();
        else if (graph_error_)
            graph_error_("This node cannot be inserted on that connection.");
        repaint();
    }

    void show_existing_node_port_choice(const std::string& node_id, const audioalgo::Connection& target,
                                        audioalgo::editor::EditorPoint position,
                                        std::vector<audioalgo::editor::InsertionPortChoice> choices) {
        juce::PopupMenu menu;
        for (std::size_t index = 0; index < choices.size(); ++index) {
            const auto label = choices[index].input_port_id + " -> " + choices[index].output_port_id;
            menu.addItem(static_cast<int>(index + 1), utf8_string(label));
        }
        const auto expected_revision = project_.revision;
        const auto expected_epoch = project_epoch_;
        juce::Component::SafePointer<GraphCanvas> safe_this(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
            [safe_this, node_id, target, position, choices = std::move(choices),
             expected_revision, expected_epoch](int selected_id) mutable {
                if (safe_this == nullptr || selected_id <= 0 ||
                    static_cast<std::size_t>(selected_id) > choices.size())
                    return;
                if (safe_this->project_epoch_ != expected_epoch ||
                    safe_this->project_.revision != expected_revision) {
                    if (safe_this->graph_error_)
                        safe_this->graph_error_("The project changed while choosing insertion ports.");
                    return;
                }
                safe_this->commit_existing_node_insertion(
                    node_id, target, position, choices[static_cast<std::size_t>(selected_id - 1)]);
            });
    }

    void update_node_drag_preview(audioalgo::editor::EditorPoint canvas_point) {
        if (dragging_node_.empty())
            return;
        const auto model_position = coordinates_.canvas_to_model(
            {canvas_point.x - drag_offset_.x, canvas_point.y - drag_offset_.y});
        node_drag_preview_position_ = audioalgo::editor::EditorPoint{
            std::max(8.0f, model_position.x), std::max(42.0f, model_position.y)};
        existing_node_insertion_target_.reset();
        node_order_swap_target_.reset();
        node_order_drop_target_.reset();
        const auto target_index = audioalgo::editor::find_visible_connection_at(
            project_, descriptors_, canvas_point, 8.0f, coordinates_, dragging_node_);
        if (target_index && *target_index < project_.connections.size())
            existing_node_insertion_target_ = project_.connections[*target_index];
        if (!existing_node_insertion_target_) {
            const auto* target = audioalgo::editor::find_topmost_node_at(
                project_.nodes, descriptors_, canvas_point, coordinates_, dragging_node_);
            if (target != nullptr) {
                node_order_drop_target_ = target->id;
                auto candidate = project_;
                if (audioalgo::editor::try_swap_adjacent_processing_nodes(
                        candidate, descriptors_, dragging_node_, target->id))
                    node_order_swap_target_ = target->id;
            }
        }
        repaint();
    }

    void update_library_drop_preview(const SourceDetails& details) {
        const auto type_id = library_drag_type(details.description);
        const auto source_epoch = library_drag_epoch_ ? library_drag_epoch_(details.sourceComponent.get()) : std::nullopt;
        if (!type_id || !source_epoch || *source_epoch != project_epoch_ ||
            !library_drop_session_.is_active() || *type_id != library_drop_session_.type_id() ||
            *source_epoch != library_drop_session_.source_epoch()) {
            library_drop_session_.cancel();
            repaint();
            return;
        }
        const auto* descriptor = descriptors_.find(*type_id);
        if (descriptor == nullptr || !audioalgo::editor::can_add_node_of_execution_kind(
                project_, descriptors_, descriptor->execution_kind)) {
            library_drop_session_.cancel();
            repaint();
            return;
        }
        const auto pointer = coordinates_.event_to_canvas({static_cast<float>(details.localPosition.x),
                                                           static_cast<float>(details.localPosition.y)});
        std::optional<audioalgo::Connection> target_connection;
        std::optional<audioalgo::editor::EditorPoint> preview_position;
        const auto target_index = audioalgo::editor::find_visible_connection_at(
            project_, descriptors_, pointer, 8.0f, coordinates_);
        if (target_index && *target_index < project_.connections.size()) {
            target_connection = project_.connections[*target_index];
            if (audioalgo::editor::decide_library_drop_route(descriptor, true) ==
                audioalgo::editor::LibraryDropRoute::insert_on_connection)
                preview_position = audioalgo::editor::library_node_origin_for_centered_drop(
                    *descriptor, pointer, coordinates_);
        }
        if (!target_connection)
            preview_position = audioalgo::editor::find_library_node_drop_position(
                project_, descriptors_, *descriptor, pointer, static_cast<float>(getWidth()),
                static_cast<float>(getHeight()), coordinates_);
        (void)library_drop_session_.update(pointer, preview_position, std::move(target_connection));
        repaint();
    }

    void paint_library_drop_preview(juce::Graphics& g) {
        const auto& position = library_drop_session_.preview_position();
        if (!library_drop_session_.is_active() || !position)
            return;
        const auto* descriptor = descriptors_.find(library_drop_session_.type_id());
        if (descriptor == nullptr)
            return;
        const audioalgo::NodeInstance preview_node{"library-drop-preview", descriptor->type_id, {},
                                                   position->x, position->y};
        const auto layout = node_geometry(preview_node);
        const auto bounds = layout.bounds;
        g.setColour(signal.withAlpha(0.16f));
        g.fillRoundedRectangle(to_juce_rect(bounds), 10.0f);
        g.setColour(signal.withAlpha(0.9f));
        g.drawRoundedRectangle(to_juce_rect(bounds), 10.0f, 1.5f);
        g.setColour(ink.withAlpha(0.85f));
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText(utf8_string(descriptor->display_name),
                   bounds.x + 12.0f, bounds.y + 8.0f, bounds.width - 24.0f, 20.0f,
                   juce::Justification::centredLeft, true);
        const auto choices = library_drop_session_.target_connection()
            ? audioalgo::editor::find_library_insertion_port_choices(
                  project_, descriptors_, descriptor->type_id, *library_drop_session_.target_connection())
            : std::vector<audioalgo::editor::InsertionPortChoice>{};
        for (const auto& port : layout.ports) {
            const auto anchor = to_juce_point(port.anchor);
            const bool compatible = std::any_of(choices.begin(), choices.end(), [&](const auto& choice) {
                return port.direction == audioalgo::PortDirection::input
                    ? choice.input_port_id == port.id : choice.output_port_id == port.id;
            });
            g.setColour(compatible ? compatible_green : signal.withAlpha(0.75f));
            g.fillEllipse(anchor.x - 6.5f, anchor.y - 6.5f, 13.0f, 13.0f);
            g.setColour(muted);
            g.setFont(juce::FontOptions(9.5f));
            g.drawFittedText(utf8_string(port.id), to_juce_rect(port.label_bounds).toNearestInt(),
                             port.direction == audioalgo::PortDirection::input
                                 ? juce::Justification::centredLeft : juce::Justification::centredRight, 1);
        }
    }

    std::optional<audioalgo::editor::PortHit> compatible_connection_target() const {
        if (!connection_gesture_.is_dragging())
            return std::nullopt;
        const auto hit = audioalgo::editor::find_topmost_port_at(
            project_.nodes, descriptors_, connection_gesture_.pointer_position(), coordinates_);
        if (!hit || hit->direction != audioalgo::PortDirection::input)
            return std::nullopt;
        const auto* target = find_node(project_, hit->node_id);
        if (target == nullptr)
            return std::nullopt;
        const auto connection = audioalgo::Connection{
            connection_gesture_.source_node_id(), connection_gesture_.source_port_id(),
            hit->node_id, hit->port_id};
        auto candidate = project_;
        if (!audioalgo::try_add_connection(candidate, connection, descriptors_).empty())
            return std::nullopt;
        return hit;
    }

    juce::Colour insertion_endpoint_colour(const std::string& node_id, const std::string& port_id,
                                           audioalgo::PortDirection direction) const {
        const auto target = active_insertion_target();
        if (!target)
            return signal;
        const bool is_endpoint =
            (direction == audioalgo::PortDirection::output && node_id == target->from_node && port_id == target->from_port) ||
            (direction == audioalgo::PortDirection::input && node_id == target->to_node && port_id == target->to_port);
        const auto choices = active_insertion_choices();
        if (is_endpoint)
            return choices.empty() ? danger : compatible_green;
        const auto preview_node_id = library_drop_session_.target_connection()
            ? std::string("library-drop-preview") : dragging_node_;
        if (node_id == preview_node_id && !choices.empty()) {
            const bool is_candidate_port = std::any_of(choices.begin(), choices.end(), [&](const auto& choice) {
                return direction == audioalgo::PortDirection::input
                    ? choice.input_port_id == port_id : choice.output_port_id == port_id;
            });
            if (is_candidate_port)
                return compatible_green;
        }
        return signal;
    }

    std::optional<audioalgo::Connection> active_insertion_target() const {
        if (library_drop_session_.target_connection())
            return library_drop_session_.target_connection();
        return existing_node_insertion_target_;
    }

    std::string active_insertion_type_id() const {
        if (library_drop_session_.target_connection())
            return library_drop_session_.type_id();
        const auto* node = find_node(project_, dragging_node_);
        return node == nullptr ? std::string{} : node->type_id;
    }

    std::optional<audioalgo::editor::EditorPoint> active_insertion_position() const {
        if (library_drop_session_.target_connection())
            return library_drop_session_.preview_position();
        return node_drag_preview_position_;
    }

    std::vector<audioalgo::editor::InsertionPortChoice> active_insertion_choices() const {
        const auto target = active_insertion_target();
        const auto type_id = active_insertion_type_id();
        if (!target || type_id.empty())
            return {};
        if (!library_drop_session_.target_connection()) {
            const bool already_connected = std::any_of(project_.connections.begin(), project_.connections.end(),
                [&](const auto& edge) { return edge.from_node == dragging_node_ || edge.to_node == dragging_node_; });
            if (already_connected)
                return {};
        }
        return audioalgo::editor::find_library_insertion_port_choices(
            project_, descriptors_, type_id, *target);
    }

    std::optional<audioalgo::NodeInstance> active_insertion_preview_node() const {
        const auto type_id = active_insertion_type_id();
        const auto position = active_insertion_position();
        if (type_id.empty() || !position)
            return std::nullopt;
        if (library_drop_session_.target_connection())
            return audioalgo::NodeInstance{"library-drop-preview", type_id, {}, position->x, position->y};
        const auto* node = find_node(project_, dragging_node_);
        if (node == nullptr)
            return std::nullopt;
        auto preview = *node;
        preview.x = position->x;
        preview.y = position->y;
        return preview;
    }

    static juce::Rectangle<float> to_juce_rect(const audioalgo::editor::EditorRect& rect) {
        return {rect.x, rect.y, rect.width, rect.height};
    }

    static juce::Point<float> to_juce_point(const audioalgo::editor::EditorPoint& point) {
        return {point.x, point.y};
    }

    audioalgo::editor::NodeVisualGeometry node_geometry(const audioalgo::NodeInstance& node) const {
        return audioalgo::editor::make_node_geometry(node, descriptors_.find(node.type_id), coordinates_);
    }

    std::optional<juce::Point<float>> port_anchor(const audioalgo::NodeInstance& node, const std::string& port_id) const {
        const auto layout = node_geometry(node);
        const auto found = std::find_if(layout.ports.begin(), layout.ports.end(), [&](const auto& port) {
            return port.id == port_id;
        });
        if (found == layout.ports.end())
            return std::nullopt;
        return to_juce_point(found->anchor);
    }

    void paint_connections(juce::Graphics& g) {
        std::optional<audioalgo::editor::NodePositionPreview> position_preview;
        if (!dragging_node_.empty() && node_drag_preview_position_)
            position_preview = audioalgo::editor::NodePositionPreview{dragging_node_, *node_drag_preview_position_};
        const auto insertion_target = active_insertion_target();
        const auto insertion_choices = active_insertion_choices();
        const auto insertion_node = active_insertion_preview_node();
        const bool split_insertion_preview = insertion_target && !insertion_choices.empty() && insertion_node;

        for (std::size_t index = 0; index < project_.connections.size(); ++index) {
            const auto& connection = project_.connections[index];
            const bool is_insertion_target = insertion_target &&
                connection.from_node == insertion_target->from_node &&
                connection.from_port == insertion_target->from_port &&
                connection.to_node == insertion_target->to_node &&
                connection.to_port == insertion_target->to_port;
            if (is_insertion_target && split_insertion_preview)
                continue;
            const auto geometry = audioalgo::editor::make_connection_geometry(
                project_, descriptors_, connection, coordinates_, position_preview);
            if (!geometry)
                continue;
            const bool selected = !is_insertion_target && selected_connection_index_ &&
                                  *selected_connection_index_ == index;
            paint_connection_curve(g, *geometry, selected ? amber : signal, selected ? 4.0f : 3.0f);
        }

        if (!split_insertion_preview)
            return;
        const auto* source = find_node(project_, insertion_target->from_node);
        const auto* destination = find_node(project_, insertion_target->to_node);
        if (source == nullptr || destination == nullptr)
            return;
        const auto source_anchor = port_anchor(*source, insertion_target->from_port);
        const auto destination_anchor = port_anchor(*destination, insertion_target->to_port);
        const auto input_anchor = port_anchor(*insertion_node, insertion_choices.front().input_port_id);
        const auto output_anchor = port_anchor(*insertion_node, insertion_choices.front().output_port_id);
        if (!source_anchor || !destination_anchor || !input_anchor || !output_anchor)
            return;
        paint_connection_curve(g, *source_anchor, *input_anchor, signal, 3.0f);
        paint_connection_curve(g, *output_anchor, *destination_anchor, signal, 3.0f);
    }

    static void paint_connection_curve(juce::Graphics& g,
                                       const audioalgo::editor::CubicConnectionGeometry& geometry,
                                       juce::Colour colour, float width) {
        juce::Path path;
        path.startNewSubPath(geometry.start.x, geometry.start.y);
        path.cubicTo(geometry.control1.x, geometry.control1.y,
                     geometry.control2.x, geometry.control2.y,
                     geometry.finish.x, geometry.finish.y);
        g.setColour(colour.withAlpha(0.85f));
        g.strokePath(path, juce::PathStrokeType(width,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }

    static void paint_connection_curve(juce::Graphics& g,
                                       juce::Point<float> start, juce::Point<float> finish,
                                       juce::Colour colour, float width) {
        const auto control_distance = std::max(48.0f, (finish.x - start.x) * 0.44f);
        const audioalgo::editor::CubicConnectionGeometry geometry{
            {start.x, start.y}, {start.x + control_distance, start.y},
            {finish.x - control_distance, finish.y}, {finish.x, finish.y}};
        paint_connection_curve(g, geometry, colour, width);
    }

    void paint_node(juce::Graphics& g, const audioalgo::NodeInstance& node,
                    const std::optional<audioalgo::editor::PortHit>& compatible_connection_target) {
        const auto layout = node_geometry(node);
        const auto bounds = to_juce_rect(layout.bounds);
        const bool selected = node.id == selection_.selected_node_id();
        const bool order_swap_target = node_order_swap_target_ && node.id == *node_order_swap_target_;
        const bool order_drop_rejected = node_order_drop_target_ && node.id == *node_order_drop_target_ &&
                                         !order_swap_target;
        g.setColour(selected ? juce::Colour(0xff304852) : panel);
        g.fillRoundedRectangle(bounds, 9.0f);
        g.setColour(order_swap_target ? compatible_green :
                    (order_drop_rejected ? danger : (selected ? amber : raised)));
        g.drawRoundedRectangle(bounds, 9.0f, selected ? 2.0f : 1.0f);

        const auto* descriptor = descriptors_.find(node.type_id);
        const auto title = descriptor == nullptr ? juce::String("Missing node") : utf8_string(descriptor->display_name);
        g.setColour(ink);
        g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        g.drawFittedText(title, to_juce_rect(layout.title_bounds).toNearestInt(), juce::Justification::centredLeft, 1);

        const auto subtitle = summary_for_node(node);
        g.setColour(muted);
        g.setFont(juce::FontOptions(11.5f));
        g.drawFittedText(subtitle, to_juce_rect(layout.summary).toNearestInt(), juce::Justification::centredLeft, 1);

        for (const auto& port : layout.ports) {
            const auto anchor = to_juce_point(port.anchor);
            const bool compatible_drag_target = compatible_connection_target &&
                compatible_connection_target->node_id == node.id &&
                compatible_connection_target->port_id == port.id &&
                compatible_connection_target->direction == port.direction;
            g.setColour(compatible_drag_target ? compatible_green :
                        insertion_endpoint_colour(node.id, port.id, port.direction));
            g.fillEllipse(anchor.x - 6.5f, anchor.y - 6.5f, 13.0f, 13.0f);
            g.setColour(muted);
            g.setFont(juce::FontOptions(9.5f));
            g.drawFittedText(utf8_string(port.id), to_juce_rect(port.label_bounds).toNearestInt(),
                             port.direction == audioalgo::PortDirection::input ? juce::Justification::centredLeft : juce::Justification::centredRight, 1);
        }
    }

    juce::String summary_for_node(const audioalgo::NodeInstance& node) const {
        juce::String subtitle;
        if (node.type_id == "builtin.gain") {
            const auto value = node.parameters.find("gain_db");
            const auto* gain = value == node.parameters.end() ? nullptr : std::get_if<double>(&value->second);
            subtitle = gain == nullptr ? "Invalid gain parameter" : juce::String(*gain, 1) + " dB";
        } else if (node.type_id == "builtin.file_source") {
            const auto value = node.parameters.find("path");
            if (value != node.parameters.end() && std::holds_alternative<std::string>(value->second)) {
                const auto path = std::filesystem::u8path(std::get<std::string>(value->second));
                const auto filename = path_to_utf8(path.filename());
                subtitle = utf8_string(filename);
            } else {
                subtitle = "Choose a WAV source";
            }
        } else {
            subtitle = "Audio output";
        }
        return subtitle;
    }

    void update_tooltip(juce::Point<float> position) {
        std::string tooltip;
        for (auto it = project_.nodes.rbegin(); it != project_.nodes.rend(); ++it) {
            const auto layout = node_geometry(*it);
            if (!to_juce_rect(layout.bounds).contains(position))
                continue;
            const auto* descriptor = descriptors_.find(it->type_id);
            const auto display_name = descriptor == nullptr ? std::string("Missing node") : descriptor->display_name;
            const auto summary = to_utf8(summary_for_node(*it));
            std::string hovered_port;
            for (const auto& port : layout.ports) {
                if (to_juce_rect(port.label_bounds).contains(position)) {
                    hovered_port = port.id;
                    break;
                }
            }
            tooltip = audioalgo::editor::build_node_tooltip_text(display_name, summary, hovered_port);
            break;
        }
        if (tooltip != tooltip_text_) {
            tooltip_text_ = std::move(tooltip);
            setTooltip(tooltip_text_.empty() ? juce::String{} : utf8_string(tooltip_text_));
        }
    }

    void connect_to(const audioalgo::NodeInstance& target, const std::string& target_port_id) {
        const auto connection = connection_gesture_.finish(target.id, target_port_id);
        if (!connection)
            return;

        std::vector<audioalgo::GraphDiagnostic> diagnostics;
        const auto committed = history_.apply(project_, [&](auto& candidate) {
            diagnostics = audioalgo::try_add_connection(candidate, *connection, descriptors_);
            return diagnostics.empty();
        });
        if (!committed) {
            if (graph_error_)
                graph_error_(diagnostics.empty() ? "The connection did not change the graph." :
                             diagnostics.front().code + ": " + diagnostics.front().message);
        } else {
            layout_changed_();
        }
        repaint();
    }

    audioalgo::ProjectModel& project_;
    audioalgo::NodeRegistry& descriptors_;
    audioalgo::editor::NodeSelectionState& selection_;
    audioalgo::editor::ProjectDirtyState& dirty_state_;
    audioalgo::editor::EditorTopologyHistory& history_;
    audioalgo::editor::CanvasCoordinateTransform coordinates_;
    std::function<void(std::string)> selection_changed_;
    std::function<void()> layout_changed_;
    std::function<void(std::string)> graph_error_;
    std::function<void(std::string, audioalgo::editor::EditorPoint, std::uint64_t, std::uint64_t,
                       std::optional<audioalgo::Connection>)> library_drop_;
    std::function<std::optional<std::uint64_t>(juce::Component*)> library_drag_epoch_;
    audioalgo::editor::LibraryDropSession library_drop_session_;
    std::uint64_t project_epoch_ = 1;
    audioalgo::editor::PortConnectionGesture connection_gesture_;
    std::string dragging_node_;
    std::optional<audioalgo::editor::EditorPoint> node_drag_preview_position_;
    std::optional<audioalgo::Connection> existing_node_insertion_target_;
    std::optional<std::string> node_order_swap_target_;
    std::optional<std::string> node_order_drop_target_;
    std::optional<std::size_t> selected_connection_index_;
    std::string tooltip_text_;
    juce::Point<float> drag_offset_;
    juce::Point<float> mouse_position_;
    juce::Point<float> last_pan_position_;
    bool panning_ = false;
};

class GainInspector final : public juce::Component {
public:
    GainInspector(audioalgo::editor::NodeSelectionState& selection, std::function<void()> changed)
        : selection_(selection), changed_(std::move(changed)) {
        addAndMakeVisible(title_);
        addAndMakeVisible(details_);
        addAndMakeVisible(gain_label_);
        addAndMakeVisible(gain_);
        title_.setColour(juce::Label::textColourId, ink);
        title_.setFont(juce::FontOptions(17.0f, juce::Font::bold));
        details_.setColour(juce::Label::textColourId, muted);
        details_.setFont(juce::FontOptions(11.5f));
        gain_label_.setText("Gain", juce::dontSendNotification);
        gain_label_.setColour(juce::Label::textColourId, muted);
        gain_.setRange(-60.0, 12.0, 0.1);
        gain_.setNumDecimalPlacesToDisplay(1);
        gain_.setTextValueSuffix(" dB");
        gain_.setSliderStyle(juce::Slider::LinearHorizontal);
        gain_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 26);
        gain_.setColour(juce::Slider::thumbColourId, amber);
        gain_.setColour(juce::Slider::trackColourId, signal);
        gain_.onValueChange = [this] { apply_gain(); };
    }

    void show_node(audioalgo::ProjectModel& project, const std::string& node_id) {
        project_ = &project;
        const bool selected = selection_.select(project, node_id);
        auto* node = selected ? mutable_node() : nullptr;
        if (node == nullptr) {
            title_.setText("Inspector", juce::dontSendNotification);
            details_.setText(node_id.empty() ? "Select a node to edit its parameters." : "Node not found.", juce::dontSendNotification);
            gain_label_.setVisible(false);
            gain_.setVisible(false);
            repaint();
            return;
        }

        const auto* descriptor = descriptors_->find(node->type_id);
        title_.setText(descriptor == nullptr ? "Missing node" : utf8_string(descriptor->display_name),
                       juce::dontSendNotification);
        details_.setText(node->type_id + "\nID " + node->id, juce::dontSendNotification);
        const bool is_gain = node->type_id == "builtin.gain";
        gain_label_.setVisible(is_gain);
        gain_.setVisible(is_gain);
        if (is_gain) {
            const auto parameter = node->parameters.find("gain_db");
            const auto value = parameter == node->parameters.end() ? 0.0 : std::get<double>(parameter->second);
            gain_.setValue(value, juce::dontSendNotification);
            gain_label_.setText(utf8_literal("Gain (−60 to +12 dB)"), juce::dontSendNotification);
        }
        resized();
        repaint();
    }

    void set_descriptor_registry(audioalgo::NodeRegistry& descriptors) noexcept { descriptors_ = &descriptors; }

    void paint(juce::Graphics& g) override {
        g.fillAll(panel);
        g.setColour(raised);
        g.drawVerticalLine(0, 0.0f, static_cast<float>(getHeight()));
    }

    void resized() override {
        auto bounds = getLocalBounds().reduced(16);
        title_.setBounds(bounds.removeFromTop(28));
        details_.setBounds(bounds.removeFromTop(66));
        bounds.removeFromTop(16);
        gain_label_.setBounds(bounds.removeFromTop(22));
        gain_.setBounds(bounds.removeFromTop(36));
    }

private:
    audioalgo::NodeInstance* mutable_node() {
        if (project_ == nullptr)
            return nullptr;
        const auto& selected_id = selection_.selected_node_id();
        const auto found = std::find_if(project_->nodes.begin(), project_->nodes.end(), [&](auto& node) { return node.id == selected_id; });
        return found == project_->nodes.end() ? nullptr : &*found;
    }

    void apply_gain() {
        auto* node = mutable_node();
        if (node == nullptr || node->type_id != "builtin.gain")
            return;
        node->parameters["gain_db"] = gain_.getValue();
        ++project_->revision;
        if (changed_)
            changed_();
    }

    audioalgo::ProjectModel* project_ = nullptr;
    audioalgo::NodeRegistry* descriptors_ = nullptr;
    audioalgo::editor::NodeSelectionState& selection_;
    std::function<void()> changed_;
    juce::Label title_;
    juce::Label details_;
    juce::Label gain_label_;
    juce::Slider gain_;
};

class EditorComponent;

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

class RunJob final : public juce::ThreadPoolJob {
public:
    RunJob(EditorComponent& target, audioalgo::NodeRegistry& descriptors,
           audioalgo::JuceProcessorRegistry& processors, audioalgo::RunRequest request,
           std::uint64_t revision);
    JobStatus runJob() override;

private:
    juce::Component::SafePointer<EditorComponent> target_;
    audioalgo::NodeRegistry& descriptors_;
    audioalgo::JuceProcessorRegistry& processors_;
    audioalgo::RunRequest request_;
    std::uint64_t revision_;
};

class EditorComponent final : public juce::Component, public juce::DragAndDropContainer {
public:
    EditorComponent()
        : canvas_(project_, descriptors_, selection_,
                  dirty_state_, history_,
                  [this](std::string id) { inspector_.show_node(project_, id); },
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
          inspector_(selection_, [this] { mark_changed(); }) {
        audioalgo::register_builtin_node_descriptors(descriptors_);
        audioalgo::register_builtin_juce_processors(processors_);
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
        addAndMakeVisible(library_title_);
        addAndMakeVisible(library_viewport_);
        library_viewport_.setViewedComponent(&library_content_, false);
        library_viewport_.setScrollBarsShown(true, false);

        style_button(open_button_);
        style_button(save_button_);
        style_button(undo_button_);
        style_button(redo_button_);
        style_button(run_button_, signal);
        style_button(play_button_, amber);
        style_button(delete_button_, danger);
        library_title_.setText("NODE LIBRARY", juce::dontSendNotification);
        library_title_.setColour(juce::Label::textColourId, muted);
        library_title_.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        status_.setColour(juce::Label::textColourId, muted);
        status_.setFont(juce::FontOptions(11.5f));

        for (const auto& entry : audioalgo::editor::build_node_library_entries(descriptors_)) {
            const auto type_id = entry.type_id;
            auto button = std::make_unique<NodeLibraryButton>(utf8_string(entry.display_name),
                type_id, [this](std::string clicked_type_id) { add_library_node(clicked_type_id); },
                [this] { return project_epoch_; });
            style_button(*button);
            library_content_.addAndMakeVisible(*button);
            library_buttons_.push_back(std::move(button));
        }

        new_button_.onClick = [this] { new_project(); };
        open_button_.onClick = [this] { open_project(); };
        save_button_.onClick = [this] { save_project(); };
        undo_button_.onClick = [this] { undo_topology(); };
        redo_button_.onClick = [this] { redo_topology(); };
        run_button_.onClick = [this] { run_project(); };
        play_button_.onClick = [this] { toggle_playback(); };
        delete_button_.onClick = [this] { canvas_.delete_selected(); };

        read_ahead_thread_.startThread();
        project_.project_id = new_id();
        project_.name = "Untitled Audio Graph";
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
        advance_project_epoch();
        active_file_chooser_.reset();
        file_chooser_completion_ = {};
        stop_playback_resources();
        read_ahead_thread_.stopThread(2000);
        worker_pool_.removeAllJobs(true, 5000);
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
        set_bounds(library_title_, layout.library_title);
        set_bounds(library_viewport_, layout.library_viewport);
        const int library_button_width = std::max(96, library_viewport_.getWidth() - 20);
        int library_y = 8;
        for (auto& button : library_buttons_) {
            button->setBounds(2, library_y, library_button_width, 34);
            library_y += 42;
        }
        library_content_.setSize(library_viewport_.getWidth() - 8, library_y);
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

    void run_finished(audioalgo::RunOutcome outcome, WaveformChannels waveform, std::uint64_t revision) {
        if (const auto* error = std::get_if<audioalgo::Diagnostic>(&outcome)) {
            set_status(utf8_string(error->code) + ": " + utf8_string(error->message), true);
            return;
        }
        const auto& result = std::get<audioalgo::RunSuccess>(outcome);
        if (project_.revision != revision) {
            set_status("Run completed for revision " + juce::String(static_cast<juce::int64>(revision)) +
                       "; current project changed, result is historical.", true);
            return;
        }
        output_file_ = result.output_file;
        waveform_.set_waveform(std::move(waveform));
        set_status("Completed run " + utf8_string(result.run_id) + utf8_literal(" · ") +
                   juce::String(static_cast<juce::int64>(result.frames)) + utf8_literal(" frames · ") +
                   utf8_string(result.output_sha256.substr(0, 12)), false);
        play_button_.setButtonText("Play output");
    }

private:
    static void style_button(juce::TextButton& button, juce::Colour accent = signal) {
        button.setColour(juce::TextButton::buttonColourId, raised);
        button.setColour(juce::TextButton::buttonOnColourId, accent.withAlpha(0.2f));
        button.setColour(juce::TextButton::textColourOffId, ink);
        button.setColour(juce::TextButton::textColourOnId, accent);
    }

    void set_status(const juce::String& message, bool is_error) {
        status_.setText(message, juce::dontSendNotification);
        status_.setColour(juce::Label::textColourId, is_error ? danger : muted);
    }

    void mark_changed() {
        dirty_state_.mark_modified();
        update_history_controls();
        canvas_.repaint();
        const auto star = project_path_.empty() ? utf8_literal(" · unsaved") :
            (dirty_state_.is_modified() ? utf8_literal(" · modified") : juce::String{});
        set_status(utf8_string(project_.name) + star, false);
    }

    void advance_project_epoch() {
        ++project_epoch_;
        if (project_epoch_ == 0)
            ++project_epoch_;
        history_.clear();
        active_file_chooser_.reset();
        file_chooser_completion_ = {};
        run_after_save_ = false;
        canvas_.set_project_epoch(project_epoch_);
        update_history_controls();
    }

    void update_history_controls() {
        undo_button_.setEnabled(history_.can_undo());
        redo_button_.setEnabled(history_.can_redo());
    }

    void undo_topology() {
        if (!history_.undo(project_))
            return;
        canvas_.select({});
        mark_changed();
        set_status("Topology change undone.", false);
    }

    void redo_topology() {
        if (!history_.redo(project_))
            return;
        canvas_.select({});
        mark_changed();
        set_status("Topology change redone.", false);
    }

    void new_project() {
        advance_project_epoch();
        project_ = audioalgo::ProjectModel{};
        project_.project_id = new_id();
        project_.name = "Untitled Audio Graph";
        project_path_.clear();
        dirty_state_.mark_saved();
        canvas_.select({});
        waveform_.set_waveform({});
        output_file_.clear();
        stop_playback_resources();
        play_button_.setButtonText("Play output");
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
        std::optional<audioalgo::editor::LibraryNodeInsertionResult> inserted;
        const auto committed = history_.apply(project_, [&](auto& candidate) {
            inserted = audioalgo::editor::try_insert_library_node_on_connection(
                candidate, descriptors_, type_id, node_id, target, position.x, position.y, choice);
            return inserted.has_value();
        });
        if (!committed) {
            set_status("This node cannot be inserted on that connection.", true);
            return;
        }
        canvas_.select(inserted->node_id);
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
        std::optional<audioalgo::editor::LibraryNodeDropResult> dropped;
        const auto committed = history_.apply(project_, [&](auto& candidate) {
            dropped = audioalgo::editor::try_drop_library_node(
                candidate, descriptors_, type_id, *node_id, pointer_position,
                static_cast<float>(canvas_.getWidth()), static_cast<float>(canvas_.getHeight()),
                canvas_.coordinate_transform());
            return dropped.has_value();
        });
        if (!committed) {
            if (!audioalgo::editor::can_add_node_of_execution_kind(project_, descriptors_, descriptor->execution_kind))
                set_status("The current run path supports one sink.", true);
            else
                set_status("No visible canvas space is available at this drop point.", true);
            return;
        }
        canvas_.select(dropped->node_id);
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
        launch_file_chooser(title, juce::File::getSpecialLocation(juce::File::userHomeDirectory),
                            "*.wav", juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                            [this, type_id, drop_pointer, source_epoch](const std::filesystem::path& selected_file) {
            if (selected_file.empty())
                return;
            if (source_epoch != project_epoch_) {
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
            auto source_path = selected_file;
            if (!project_path_.empty()) {
                std::error_code ignored;
                const auto relative = std::filesystem::relative(selected_file, project_path_.parent_path(), ignored);
                if (!ignored && !relative.empty() && *relative.begin() != "..")
                    source_path = relative;
            }
            const auto path_parameter = std::find_if(descriptor->parameters.begin(), descriptor->parameters.end(), [](const auto& parameter) {
                return parameter.kind == audioalgo::ParameterKind::path;
            });
            if (path_parameter == descriptor->parameters.end()) {
                set_status("This source descriptor does not declare a path parameter.", true);
                return;
            }
            std::optional<audioalgo::editor::LibraryNodeDropResult> dropped;
            const auto committed = history_.apply(project_, [&](auto& candidate) {
                if (drop_pointer) {
                    dropped = audioalgo::editor::try_add_source_node_from_drop(
                        candidate, descriptors_, type_id, *node_id, path_to_utf8(source_path), *drop_pointer,
                        static_cast<float>(canvas_.getWidth()), static_cast<float>(canvas_.getHeight()),
                        source_epoch, project_epoch_, canvas_.coordinate_transform());
                    return dropped.has_value();
                }

                const auto position = audioalgo::editor::find_node_placement(
                    candidate.nodes, descriptors_, *descriptor, static_cast<float>(canvas_.getWidth()),
                    static_cast<float>(canvas_.getHeight()), canvas_.coordinate_transform());
                if (!position)
                    return false;
                auto node = audioalgo::editor::make_node_instance(
                    descriptors_, candidate.nodes, type_id, *node_id, position->x, position->y);
                if (!node)
                    return false;
                node->parameters[path_parameter->id] = path_to_utf8(source_path);
                candidate.nodes.push_back(std::move(*node));
                ++candidate.revision;
                return true;
            });
            if (!committed) {
                set_status(drop_pointer ? "The source could not be placed at the original drop point." :
                           "No visible canvas space is available for another node.", true);
                return;
            }
            canvas_.select(*node_id);
            mark_changed();
            set_status("Source added. Connect its output to a processing node.", false);
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
        const auto committed = history_.apply(project_, [&](auto& candidate) {
            return audioalgo::editor::try_add_node_from_descriptor(
                candidate, descriptors_, type_id, *node_id, position->x, position->y);
        });
        if (!committed) {
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

    bool save_to(const std::filesystem::path& path) {
        const auto result = audioalgo::save_project_file(path, project_);
        if (const auto* error = std::get_if<audioalgo::Diagnostic>(&result)) {
            set_status(utf8_string(error->code) + ": " + utf8_string(error->message), true);
            return false;
        }
        project_path_ = path;
        const auto project_name = path_to_utf8(path.stem());
        project_.name = project_name;
        dirty_state_.mark_saved();
        set_status("Saved " + utf8_string(path_to_utf8(path.filename())), false);
        return true;
    }

    void launch_file_chooser(const juce::String& title, const juce::File& initial_file,
                             const juce::String& pattern, int flags,
                             std::function<void(const std::filesystem::path&)> completion) {
        if (active_file_chooser_ != nullptr) {
            set_status("Finish the open file selection first.", true);
            return;
        }
        file_chooser_completion_ = std::move(completion);
        active_file_chooser_ = std::make_unique<juce::FileChooser>(title, initial_file, pattern, true, false, this);
        juce::Component::SafePointer<EditorComponent> safe_this(this);
        active_file_chooser_->launchAsync(flags, [safe_this](const juce::FileChooser& chooser) {
            if (safe_this == nullptr)
                return;
            const auto result = chooser.getResult();
            const auto selected = result == juce::File() ? std::filesystem::path{} : path_from_juce_file(result);
            juce::MessageManager::callAsync([safe_this, selected] {
                if (safe_this != nullptr)
                    safe_this->finish_file_chooser(selected);
            });
        });
    }

    void finish_file_chooser(const std::filesystem::path& selected) {
        active_file_chooser_.reset();
        auto completion = std::move(file_chooser_completion_);
        file_chooser_completion_ = {};
        if (completion)
            completion(selected);
    }

    void save_project(bool run_after_save = false) {
        if (!project_path_.empty()) {
            if (save_to(project_path_) && run_after_save)
                run_project();
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
            if (save_to(path) && should_run)
                run_project();
        });
    }

    void open_project() {
        launch_file_chooser("Open AudioAlgo project", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
                            "*.aaproj", juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                            [this](const std::filesystem::path& path) {
            if (path.empty())
                return;
            const auto result = audioalgo::load_project_file(path);
            if (const auto* error = std::get_if<audioalgo::Diagnostic>(&result)) {
                set_status(utf8_string(error->code) + ": " + utf8_string(error->message), true);
                return;
            }
            advance_project_epoch();
            project_ = std::get<audioalgo::ProjectModel>(result);
            project_path_ = path;
            dirty_state_.mark_saved();
            canvas_.select({});
            waveform_.set_waveform({});
            output_file_.clear();
            set_status("Opened " + utf8_string(path_to_utf8(path.filename())), false);
        });
    }

    void load_staged_demo() {
        const auto app_directory = juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory();
        const auto package_directory = app_directory.getChildFile("examples").getChildFile("gain_demo");
        const auto package_project = package_directory.getChildFile("gain_demo.aaproj");
        if (!package_project.existsAsFile())
            return;
        const auto user_directory = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("AudioAlgo").getChildFile("Examples").getChildFile("gain_demo");
        if (!user_directory.createDirectory()) {
            set_status("Could not prepare the writable demo folder in Documents.", true);
            return;
        }
        for (const auto* filename : {"input.wav", "gain_demo.aaproj", "demo_manifest.json"}) {
            const auto source = package_directory.getChildFile(filename);
            const auto destination = user_directory.getChildFile(filename);
            if (source.existsAsFile() && !destination.existsAsFile() && !source.copyFileTo(destination)) {
                set_status("Could not copy the demo asset " + juce::String(filename), true);
                return;
            }
        }
        const auto path = path_from_juce_file(user_directory.getChildFile("gain_demo.aaproj"));
        const auto loaded = audioalgo::load_project_file(path);
        if (const auto* project = std::get_if<audioalgo::ProjectModel>(&loaded)) {
            advance_project_epoch();
            project_ = *project;
            project_path_ = path;
            dirty_state_.mark_saved();
            canvas_.select("gain");
            set_status(utf8_literal("Ready · Stereo Gain Demo · source files are in Documents/AudioAlgo/Examples"), false);
        }
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
        const auto run_root = project_path_.parent_path() / "runs";
        const auto output_directory = run_root / ("run-" + new_id());
        audioalgo::RunRequest request{project_, project_path_, output_directory, 512};
        set_status("Rendering revision " + juce::String(static_cast<juce::int64>(revision)) + utf8_literal(" in the background…"), false);
        worker_pool_.addJob(new RunJob(*this, descriptors_, processors_, std::move(request), revision), true);
    }

    void toggle_playback() {
        if (transport_.isPlaying()) {
            transport_.stop();
            play_button_.setButtonText("Resume");
            set_status("Paused at " + juce::String(transport_.getCurrentPosition(), 2) + " s", false);
            return;
        }
        if (output_file_.empty()) {
            set_status("Run the graph before previewing audio.", true);
            return;
        }
        if (reader_source_ == nullptr) {
            audio_format_manager_.registerBasicFormats();
            auto* reader = audio_format_manager_.createReaderFor(to_juce_file(output_file_));
            if (reader == nullptr) {
                set_status("Could not open the rendered WAV for playback.", true);
                return;
            }
            const auto sample_rate = reader->sampleRate;
            reader_source_ = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
            transport_.setSource(reader_source_.get(), 32768, &read_ahead_thread_, sample_rate);
        }
        if (!audio_device_ready_) {
            const auto error = audio_device_manager_.initialise(0, 2, nullptr, true);
            if (error.isNotEmpty()) {
                audio_device_manager_.closeAudioDevice();
                set_status("Audio output unavailable: " + error, true);
                play_button_.setButtonText("Play output");
                return;
            }
            audio_device_manager_.addAudioCallback(&audio_source_player_);
            audio_source_player_.setSource(&transport_);
            audio_device_ready_ = true;
        }
        if (transport_.getCurrentPosition() >= transport_.getLengthInSeconds())
            transport_.setPosition(0.0);
        transport_.start();
        play_button_.setButtonText("Pause");
        set_status("Playing rendered output", false);
    }

    void stop_playback_resources() {
        transport_.stop();
        transport_.setSource(nullptr);
        reader_source_.reset();
        audio_source_player_.setSource(nullptr);
        if (audio_device_ready_) {
            audio_device_manager_.removeAudioCallback(&audio_source_player_);
            audio_device_manager_.closeAudioDevice();
            audio_device_ready_ = false;
        }
    }

    audioalgo::NodeRegistry descriptors_;
    audioalgo::JuceProcessorRegistry processors_;
    audioalgo::ProjectModel project_;
    std::filesystem::path project_path_;
    std::filesystem::path output_file_;
    std::uint64_t project_epoch_ = 1;
    audioalgo::editor::NodeSelectionState selection_;
    audioalgo::editor::ProjectDirtyState dirty_state_;
    audioalgo::editor::EditorTopologyHistory history_;
    GraphCanvas canvas_;
    GainInspector inspector_;
    WaveformView waveform_;
    juce::Label library_title_;
    juce::Component library_content_;
    juce::Viewport library_viewport_;
    std::vector<std::unique_ptr<juce::TextButton>> library_buttons_;
    juce::Label status_;
    juce::TextButton new_button_{"New"};
    juce::TextButton open_button_{"Open"};
    juce::TextButton save_button_{"Save"};
    juce::TextButton undo_button_{"Undo"};
    juce::TextButton redo_button_{"Redo"};
    juce::TextButton run_button_{"Run"};
    juce::TextButton play_button_{"Play output"};
    juce::TextButton delete_button_{"Delete selected"};
    juce::ThreadPool worker_pool_{1};
    juce::AudioDeviceManager audio_device_manager_;
    juce::AudioSourcePlayer audio_source_player_;
    juce::AudioTransportSource transport_;
    juce::AudioFormatManager audio_format_manager_;
    juce::TimeSliceThread read_ahead_thread_{"AudioAlgo preview read-ahead"};
    std::unique_ptr<juce::AudioFormatReaderSource> reader_source_;
    bool audio_device_ready_ = false;
    std::function<void(const std::filesystem::path&)> file_chooser_completion_;
    std::unique_ptr<juce::FileChooser> active_file_chooser_;
    bool run_after_save_ = false;
};

RunJob::RunJob(EditorComponent& target, audioalgo::NodeRegistry& descriptors,
               audioalgo::JuceProcessorRegistry& processors, audioalgo::RunRequest request,
               std::uint64_t revision)
    : ThreadPoolJob("AudioAlgo offline render"), target_(&target), descriptors_(descriptors), processors_(processors),
      request_(std::move(request)), revision_(revision) {}

juce::ThreadPoolJob::JobStatus RunJob::runJob() {
    std::error_code filesystem_error;
    std::filesystem::create_directories(request_.output_directory.parent_path(), filesystem_error);
    audioalgo::RunService service(descriptors_, processors_);
    auto outcome = filesystem_error
        ? audioalgo::RunOutcome(audioalgo::Diagnostic{"run_directory_create_failed", filesystem_error.message()})
        : service.run(request_);

    WaveformChannels waveform;
    if (const auto* success = std::get_if<audioalgo::RunSuccess>(&outcome)) {
        const auto audio = audioalgo::read_wav_file(success->output_file);
        if (const auto* buffer = std::get_if<audioalgo::AudioBuffer>(&audio))
            waveform = make_waveform(*buffer);
    }

    auto target = target_;
    juce::MessageManager::callAsync([target, outcome = std::move(outcome), waveform = std::move(waveform), revision = revision_]() mutable {
        if (target != nullptr)
            target->run_finished(std::move(outcome), std::move(waveform), revision);
    });
    return jobHasFinished;
}

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow() : DocumentWindow("AudioAlgo Editor", background, DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        setContentOwned(new EditorComponent(), true);
        const auto* primary_display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        const auto initial_size = primary_display == nullptr
            ? audioalgo::editor::default_editor_window_size
            : audioalgo::editor::make_initial_editor_window_size(
                  primary_display->userArea.getWidth(), primary_display->userArea.getHeight());
        centreWithSize(initial_size.width, initial_size.height);
        setResizable(true, true);
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class AudioAlgoApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "AudioAlgo Editor"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String&) override {
        window_ = std::make_unique<MainWindow>();
        if (getCommandLineParameters().contains("--smoke-test"))
            juce::Timer::callAfterDelay(250, [] { juce::JUCEApplication::getInstance()->quit(); });
    }

    void shutdown() override { window_.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<MainWindow> window_;
};

} // namespace

START_JUCE_APPLICATION(AudioAlgoApplication)
