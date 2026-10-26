#pragma once
#include "EditorUiSupport.h"
#include "EditorGeometry.h"
#include "EditorNodeCreation.h"
#include "EditorNodeDragDrop.h"
#include "EditorTopologyCommands.h"
#include "EditorNodeSummary.h"
#include "audio_editor_application/EditorController.h"
#include <cmath>
#include <functional>
namespace audioalgo::editor {
class GraphCanvas final : public juce::Component, public juce::SettableTooltipClient,
                          public juce::DragAndDropTarget {
public:
    GraphCanvas(const audioalgo::ProjectModel& project, const audioalgo::NodeCatalog& descriptors,
                audioalgo::editor::NodeSelectionState& selection,
                audioalgo::editor::EditorController& controller,
                std::function<void(std::string)> selection_changed,
                std::function<void()> layout_changed,
                std::function<void(std::string)> graph_error,
                std::function<void(std::string, audioalgo::editor::EditorPoint, std::uint64_t, std::uint64_t,
                                   std::optional<audioalgo::Connection>)> library_drop,
                std::function<std::optional<std::uint64_t>(juce::Component*)> library_drag_epoch)
        : project_(project), descriptors_(descriptors), selection_(selection), controller_(controller),
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
        cancel_marquee();
        coordinates_.reset_pan();
        setMouseCursor(juce::MouseCursor::NormalCursor);
        selected_connection_index_.reset();
        repaint();
    }

    void finish_library_drag(std::uint64_t source_epoch) noexcept {
        library_drop_session_.finish_drag(source_epoch);
        repaint();
    }
    void cancel_library_drag() noexcept { library_drop_session_.cancel(); repaint(); }

    void paint(juce::Graphics& g) override {
        g.fillAll(background);
        g.setColour(grid);
        const auto pan = coordinates_.pan_offset();
        const auto spacing = 28.0f * coordinates_.zoom_scale();
        const auto grid_phase = [&](float offset) {
            auto phase = std::fmod(offset, spacing);
            if (phase < 0.0f)
                phase += spacing;
            return phase - spacing;
        };
        for (auto x = grid_phase(pan.x); x < getWidth(); x += spacing)
            for (auto y = grid_phase(pan.y); y < getHeight(); y += spacing)
                g.fillEllipse(x, y, 1.5f, 1.5f);

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
                const auto bend = 64.0f * coordinates_.zoom_scale();
                path.cubicTo(start->translated(bend, 0.0f), preview_pointer.translated(-bend, 0.0f), preview_pointer);
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
        if (marquee_start_ && marquee_dragged_) {
            g.setColour(signal.withAlpha(0.12f));
            g.fillRect(marquee_rect_);
            g.setColour(signal.withAlpha(0.8f));
            g.drawRect(marquee_rect_, 1.0f);
        }
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
            cancel_marquee();
            panning_ = true;
            last_pan_position_ = event.position;
            connection_gesture_.cancel();
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            return;
        }
        grabKeyboardFocus();
        // Right drags always belong to the marquee, even over nodes, sockets or edges.
        if (event.mods.isRightButtonDown()) {
            connection_gesture_.cancel();
            dragging_node_.clear();
            node_drag_preview_position_.reset();
            existing_node_insertion_target_.reset();
            node_order_swap_target_.reset();
            node_order_drop_target_.reset();
            selected_connection_index_.reset();
            begin_marquee(event.position, true);
            return;
        }
        if (!event.mods.isLeftButtonDown())
            return;
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
            (void)selection_.select(project_, {});
            dragging_node_.clear();
            selection_changed_({});
            repaint();
            return;
        }
        // Left press on empty canvas starts a rubber-band selection; a click
        // without dragging clears the selection on release.
        begin_marquee(event.position, false);
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
        if (marquee_start_) {
            update_marquee(event.position);
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
        if (marquee_start_) {
            update_marquee(event.position);
            const auto band = marquee_rect_;
            const bool is_click = !marquee_dragged_;
            const bool preserve_click = marquee_right_button_;
            cancel_marquee();
            if (is_click) {
                if (!preserve_click) {
                    (void)selection_.select(project_, {});
                    selection_changed_({});
                }
            } else {
                const auto ids = audioalgo::editor::nodes_intersecting_rect(
                    project_.nodes, descriptors_,
                    {band.getX(), band.getY(), band.getWidth(), band.getHeight()}, coordinates_);
                (void)selection_.select_many(project_, ids);
                selection_changed_(selection_.selected_node_id());
            }
            repaint();
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
                const auto swapped = controller_.swap_processing_nodes(dragged_node_id, target_node_id);
                if (swapped.status == audioalgo::editor::ProjectEditStatus::applied) {
                    (void)selection_.select(project_, dragged_node_id);
                    selection_changed_(selection_.selected_node_id());
                    layout_changed_();
                }
            } else if (node_order_drop_target_) {
                if (graph_error_)
                    graph_error_("Only adjacent processing nodes with a compatible, unbranched path can swap order.");
            } else if (controller_.move_node_layout(dragging_node_, position.x, position.y).status ==
                       audioalgo::editor::ProjectEditStatus::applied) {
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

    // Wheel zoom keeps the model point under the pointer fixed; gesture drags
    // keep their canvas-space offsets, so zoom is deferred until they finish.
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override {
        if (panning_ || !dragging_node_.empty() || connection_gesture_.is_active() || marquee_start_)
            return;
        const auto direction = wheel.isReversed ? -wheel.deltaY : wheel.deltaY;
        coordinates_.zoom_around({event.position.x, event.position.y}, std::pow(1.25f, direction));
        mouse_position_ = to_juce_point(coordinates_.event_to_canvas({event.position.x, event.position.y}));
        update_tooltip(mouse_position_);
        repaint();
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (connection_gesture_.cancel_on_escape(key.getKeyCode() == juce::KeyPress::escapeKey)) {
            repaint();
            return true;
        }
        if (marquee_start_ && key.getKeyCode() == juce::KeyPress::escapeKey) {
            cancel_marquee();
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

    void focusLost(FocusChangeType) override {
        cancel_marquee();
        repaint();
    }

    void visibilityChanged() override {
        if (!isVisible()) {
            cancel_marquee();
            repaint();
        }
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
            if (controller_.disconnect(selected_connection).status == audioalgo::editor::ProjectEditStatus::applied) {
                layout_changed_();
                repaint();
            }
            return;
        }
        const auto selected_ids = selection_.selected_node_ids();
        if (selected_ids.empty())
            return;
        if (controller_.delete_nodes(selected_ids).status != audioalgo::editor::ProjectEditStatus::applied)
            return;
        selected_connection_index_.reset();
        (void)selection_.select(project_, {});
        selection_changed_({});
        layout_changed_();
        repaint();
    }

private:
    void begin_marquee(juce::Point<float> position, bool right_button) {
        marquee_start_ = position;
        marquee_rect_ = {};
        marquee_right_button_ = right_button;
        marquee_dragged_ = false;
        repaint();
    }

    void update_marquee(juce::Point<float> position) {
        if (position.getDistanceFrom(*marquee_start_) > 6.0f)
            marquee_dragged_ = true;
        marquee_rect_ = juce::Rectangle<float>(*marquee_start_, position);
    }

    void cancel_marquee() noexcept {
        marquee_start_.reset();
        marquee_rect_ = {};
        marquee_right_button_ = false;
        marquee_dragged_ = false;
    }

    void commit_existing_node_insertion(const std::string& node_id, const audioalgo::Connection& target,
                                        audioalgo::editor::EditorPoint position,
                                        const audioalgo::editor::InsertionPortChoice& choice) {
        const auto inserted = controller_.insert_existing_node(node_id, target, position.x, position.y, choice);
        if (inserted.status == audioalgo::editor::ProjectEditStatus::applied)
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
        node_drag_preview_position_ = model_position;
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
        const auto scale = coordinates_.zoom_scale();
        g.setColour(signal.withAlpha(0.16f));
        g.fillRoundedRectangle(to_juce_rect(bounds), 10.0f * scale);
        g.setColour(signal.withAlpha(0.9f));
        g.drawRoundedRectangle(to_juce_rect(bounds), 10.0f * scale, 1.5f);
        g.setColour(ink.withAlpha(0.85f));
        g.setFont(juce::FontOptions(12.0f * scale, juce::Font::bold));
        g.drawText(utf8_string(descriptor->display_name), to_juce_rect(layout.title_bounds),
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
            const auto radius = 6.5f * scale;
            g.fillEllipse(anchor.x - radius, anchor.y - radius, 2 * radius, 2 * radius);
            g.setColour(muted);
            g.setFont(juce::FontOptions(9.5f * scale));
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
            paint_connection_curve(g, *geometry, selected ? ink : signal, selected ? 4.0f : 3.0f);
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

    void paint_connection_curve(juce::Graphics& g,
                                juce::Point<float> start, juce::Point<float> finish,
                                juce::Colour colour, float width) {
        const auto control_distance = std::max(48.0f * coordinates_.zoom_scale(), (finish.x - start.x) * 0.44f);
        const audioalgo::editor::CubicConnectionGeometry geometry{
            {start.x, start.y}, {start.x + control_distance, start.y},
            {finish.x - control_distance, finish.y}, {finish.x, finish.y}};
        paint_connection_curve(g, geometry, colour, width);
    }

    void paint_node(juce::Graphics& g, const audioalgo::NodeInstance& node,
                    const std::optional<audioalgo::editor::PortHit>& compatible_connection_target) {
        const auto layout = node_geometry(node);
        const auto bounds = to_juce_rect(layout.bounds);
        const auto scale = coordinates_.zoom_scale();
        const bool selected = selection_.is_selected(node.id);
        const bool order_swap_target = node_order_swap_target_ && node.id == *node_order_swap_target_;
        const bool order_drop_rejected = node_order_drop_target_ && node.id == *node_order_drop_target_ &&
                                         !order_swap_target;
        g.setColour(selected ? selection : raised);
        g.fillRoundedRectangle(bounds, 9.0f * scale);
        g.setColour(order_swap_target ? compatible_green :
                    (order_drop_rejected ? danger : (selected ? signal : border)));
        g.drawRoundedRectangle(bounds, 9.0f * scale, selected ? 2.0f : 1.0f);

        const auto* descriptor = descriptors_.find(node.type_id);
        const auto title = descriptor == nullptr ? juce::String("Missing node") : utf8_string(descriptor->display_name);
        g.setColour(ink);
        g.setFont(juce::FontOptions(15.0f * scale, juce::Font::bold));
        g.drawFittedText(title, to_juce_rect(layout.title_bounds).toNearestInt(), juce::Justification::centredLeft, 1);

        const auto subtitle = summary_for_node(node);
        g.setColour(muted);
        g.setFont(juce::FontOptions(11.5f * scale));
        g.drawFittedText(subtitle, to_juce_rect(layout.summary).toNearestInt(), juce::Justification::centredLeft, 1);

        for (const auto& port : layout.ports) {
            const auto anchor = to_juce_point(port.anchor);
            const bool compatible_drag_target = compatible_connection_target &&
                compatible_connection_target->node_id == node.id &&
                compatible_connection_target->port_id == port.id &&
                compatible_connection_target->direction == port.direction;
            g.setColour(compatible_drag_target ? compatible_green :
                        insertion_endpoint_colour(node.id, port.id, port.direction));
            const auto radius = 6.5f * scale;
            g.fillEllipse(anchor.x - radius, anchor.y - radius, 2 * radius, 2 * radius);
            g.setColour(muted);
            g.setFont(juce::FontOptions(9.5f * scale));
            g.drawFittedText(utf8_string(port.id), to_juce_rect(port.label_bounds).toNearestInt(),
                             port.direction == audioalgo::PortDirection::input ? juce::Justification::centredLeft : juce::Justification::centredRight, 1);
        }
    }

    juce::String summary_for_node(const audioalgo::NodeInstance& node) const {
        return audioalgo::editor::node_summary(node, descriptors_.find(node.type_id));
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

        const auto connected = controller_.connect(*connection);
        if (connected.status != audioalgo::editor::ProjectEditStatus::applied) {
            if (graph_error_)
                graph_error_(connected.diagnostics.empty() ? "The connection did not change the graph." :
                             connected.diagnostics.front().code + ": " + connected.diagnostics.front().message);
        } else {
            layout_changed_();
        }
        repaint();
    }

    const audioalgo::ProjectModel& project_;
    const audioalgo::NodeCatalog& descriptors_;
    audioalgo::editor::NodeSelectionState& selection_;
    audioalgo::editor::EditorController& controller_;
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
    std::optional<juce::Point<float>> marquee_start_;
    juce::Rectangle<float> marquee_rect_;
    bool marquee_right_button_ = false;
    bool marquee_dragged_ = false;
};


} // namespace audioalgo::editor
