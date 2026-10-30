#pragma once

#include "EditorGeometry.h"
#include "EditorNodeCreation.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace audioalgo::editor {

class LibraryNodeDragGesture {
public:
    enum class Release { none, click, drag };

    void begin(EditorPoint position) noexcept {
        origin_ = position;
        active_ = true;
        drag_started_ = false;
    }

    [[nodiscard]] bool move(EditorPoint position) noexcept {
        if (!active_ || drag_started_)
            return false;
        const auto dx = position.x - origin_.x;
        const auto dy = position.y - origin_.y;
        if (dx * dx + dy * dy <= threshold_ * threshold_)
            return false;
        drag_started_ = true;
        return true;
    }

    [[nodiscard]] Release finish(bool released_inside_source) noexcept {
        if (!active_)
            return Release::none;
        const auto was_drag = drag_started_;
        active_ = false;
        drag_started_ = false;
        return was_drag ? Release::drag : (released_inside_source ? Release::click : Release::none);
    }

    [[nodiscard]] bool is_active() const noexcept { return active_; }

private:
    static constexpr float threshold_ = 6.0f;
    EditorPoint origin_;
    bool active_ = false;
    bool drag_started_ = false;
};

class LibraryDropSession {
public:
    [[nodiscard]] bool begin(std::string type_id, std::uint64_t source_epoch, std::uint64_t project_revision) {
        if (type_id.empty())
            return false;
        type_id_ = std::move(type_id);
        source_epoch_ = source_epoch;
        project_revision_ = project_revision;
        pointer_position_ = {};
        preview_position_.reset();
        target_connection_.reset();
        active_ = true;
        over_canvas_ = false;
        return true;
    }

    [[nodiscard]] bool update(EditorPoint pointer_position, std::optional<EditorPoint> preview_position,
                              std::optional<Connection> target_connection = std::nullopt) {
        if (!active_)
            return false;
        pointer_position_ = pointer_position;
        preview_position_ = preview_position;
        target_connection_ = std::move(target_connection);
        over_canvas_ = true;
        return true;
    }

    void leave_canvas() noexcept {
        over_canvas_ = false;
        preview_position_.reset();
        target_connection_.reset();
    }

    void finish_drag(std::uint64_t source_epoch) noexcept {
        if (active_ && source_epoch_ == source_epoch)
            cancel();
    }

    void cancel() noexcept {
        active_ = false;
        project_revision_ = 0;
        over_canvas_ = false;
        type_id_.clear();
        preview_position_.reset();
        target_connection_.reset();
        pointer_position_ = {};
    }

    [[nodiscard]] bool can_deliver_drop(std::uint64_t current_epoch, std::uint64_t current_revision) const noexcept {
        return active_ && over_canvas_ &&
               source_epoch_ == current_epoch && project_revision_ == current_revision;
    }
    [[nodiscard]] bool is_active() const noexcept { return active_; }
    [[nodiscard]] const std::string& type_id() const noexcept { return type_id_; }
    [[nodiscard]] std::uint64_t source_epoch() const noexcept { return source_epoch_; }
    [[nodiscard]] std::uint64_t project_revision() const noexcept { return project_revision_; }
    [[nodiscard]] EditorPoint pointer_position() const noexcept { return pointer_position_; }
    [[nodiscard]] const std::optional<EditorPoint>& preview_position() const noexcept { return preview_position_; }
    [[nodiscard]] const std::optional<Connection>& target_connection() const noexcept { return target_connection_; }

private:
    std::string type_id_;
    std::uint64_t source_epoch_ = 0;
    std::uint64_t project_revision_ = 0;
    EditorPoint pointer_position_;
    std::optional<EditorPoint> preview_position_;
    std::optional<Connection> target_connection_;
    bool active_ = false;
    bool over_canvas_ = false;
};

[[nodiscard]] inline EditorPoint library_node_origin_for_centered_drop(
    const NodeDescriptor& descriptor, EditorPoint pointer_position,
    const CanvasCoordinateTransform& transform = {}) {
    const NodeInstance preview{"library-drop-preview", descriptor.type_id, {}, 0.0, 0.0};
    const auto bounds = make_node_geometry(preview, &descriptor, transform).bounds;
    return transform.canvas_to_model({pointer_position.x - bounds.width * 0.5f,
                                      pointer_position.y - bounds.height * 0.5f});
}

class PortConnectionGesture {
public:
    [[nodiscard]] bool begin(std::string source_node_id, std::string source_port_id,
                             EditorPoint pointer_position = {}) {
        return begin(std::move(source_node_id), std::move(source_port_id),
                     audioalgo::PortDirection::output, pointer_position);
    }

    [[nodiscard]] bool begin(std::string start_node_id, std::string start_port_id,
                             audioalgo::PortDirection start_direction,
                             EditorPoint pointer_position = {}) {
        if (start_node_id.empty() || start_port_id.empty())
            return false;
        start_node_id_ = std::move(start_node_id);
        start_port_id_ = std::move(start_port_id);
        start_direction_ = start_direction;
        connection_to_replace_.reset();
        reconnect_source_endpoint_ = false;
        pointer_position_ = pointer_position;
        drag_origin_position_ = pointer_position;
        drag_position_ = pointer_position;
        active_ = true;
        dragging_ = false;
        return true;
    }

    [[nodiscard]] bool begin_reconnect(Connection connection, EditorPoint pointer_position = {}) {
        if (connection.from_node.empty() || connection.from_port.empty() ||
            connection.to_node.empty() || connection.to_port.empty())
            return false;
        start_node_id_ = connection.to_node;
        start_port_id_ = connection.to_port;
        start_direction_ = audioalgo::PortDirection::input;
        pointer_position_ = pointer_position;
        drag_origin_position_ = pointer_position;
        drag_position_ = pointer_position;
        connection_to_replace_ = std::move(connection);
        reconnect_source_endpoint_ = false;
        active_ = true;
        dragging_ = false;
        return true;
    }

    [[nodiscard]] bool begin_source_reconnect(Connection connection, EditorPoint pointer_position = {}) {
        if (connection.from_node.empty() || connection.from_port.empty() ||
            connection.to_node.empty() || connection.to_port.empty())
            return false;
        start_node_id_ = connection.from_node;
        start_port_id_ = connection.from_port;
        start_direction_ = audioalgo::PortDirection::output;
        pointer_position_ = pointer_position;
        drag_origin_position_ = pointer_position;
        drag_position_ = pointer_position;
        connection_to_replace_ = std::move(connection);
        reconnect_source_endpoint_ = true;
        active_ = true;
        dragging_ = false;
        return true;
    }

    [[nodiscard]] bool update_pointer(EditorPoint pointer_position) noexcept {
        if (!active_)
            return false;
        pointer_position_ = pointer_position;
        drag_position_ = pointer_position;
        return true;
    }

    [[nodiscard]] bool update_pointer(EditorPoint pointer_position, EditorPoint drag_position) noexcept {
        if (!active_)
            return false;
        pointer_position_ = pointer_position;
        drag_position_ = drag_position;
        return true;
    }

    void set_drag_origin(EditorPoint drag_origin_position) noexcept {
        if (active_)
            drag_origin_position_ = drag_origin_position;
    }

    void mark_drag_started() noexcept {
        const auto dx = drag_position_.x - drag_origin_position_.x;
        const auto dy = drag_position_.y - drag_origin_position_.y;
        if (active_ && dx * dx + dy * dy > drag_threshold_ * drag_threshold_)
            dragging_ = true;
    }

    [[nodiscard]] bool cancel_on_escape(bool escape_pressed) noexcept {
        if (!active_ || !escape_pressed)
            return false;
        cancel();
        return true;
    }

    [[nodiscard]] std::optional<Connection> finish(const std::string& target_node_id,
                                                   const std::string& target_port_id) {
        return finish(target_node_id, target_port_id, target_direction());
    }

    [[nodiscard]] std::optional<Connection> connection_to(const std::string& target_node_id,
                                                          const std::string& target_port_id,
                                                          audioalgo::PortDirection target_direction) const {
        if (!active_ || target_node_id.empty() || target_port_id.empty())
            return std::nullopt;
        if (connection_to_replace_ && reconnect_source_endpoint_ &&
            target_direction == audioalgo::PortDirection::output) {
            auto replacement = *connection_to_replace_;
            replacement.from_node = target_node_id;
            replacement.from_port = target_port_id;
            return replacement;
        }
        if (connection_to_replace_ && !reconnect_source_endpoint_ &&
            target_direction == audioalgo::PortDirection::input) {
            auto replacement = *connection_to_replace_;
            replacement.to_node = target_node_id;
            replacement.to_port = target_port_id;
            return replacement;
        }
        if (start_direction_ == audioalgo::PortDirection::output &&
            target_direction == audioalgo::PortDirection::input)
            return Connection{start_node_id_, start_port_id_, target_node_id, target_port_id};
        if (start_direction_ == audioalgo::PortDirection::input &&
            target_direction == audioalgo::PortDirection::output)
            return Connection{target_node_id, target_port_id, start_node_id_, start_port_id_};
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Connection> finish(const std::string& target_node_id,
                                                   const std::string& target_port_id,
                                                   audioalgo::PortDirection target_direction) {
        if (!active_ || target_node_id.empty() || target_port_id.empty())
            return std::nullopt;
        auto connection = connection_to(target_node_id, target_port_id, target_direction);
        cancel();
        return connection;
    }

    void cancel() noexcept {
        active_ = false;
        dragging_ = false;
        start_node_id_.clear();
        start_port_id_.clear();
        start_direction_ = audioalgo::PortDirection::output;
        connection_to_replace_.reset();
        reconnect_source_endpoint_ = false;
        pointer_position_ = {};
        drag_origin_position_ = {};
        drag_position_ = {};
    }

    [[nodiscard]] bool is_active() const noexcept { return active_; }
    [[nodiscard]] bool is_dragging() const noexcept { return active_ && dragging_; }
    [[nodiscard]] audioalgo::PortDirection start_direction() const noexcept { return start_direction_; }
    [[nodiscard]] bool is_reconnecting() const noexcept { return connection_to_replace_.has_value(); }
    [[nodiscard]] bool is_reconnecting_source() const noexcept {
        return connection_to_replace_.has_value() && reconnect_source_endpoint_;
    }
    [[nodiscard]] audioalgo::PortDirection target_direction() const noexcept {
        if (connection_to_replace_)
            return reconnect_source_endpoint_ ? audioalgo::PortDirection::output : audioalgo::PortDirection::input;
        return start_direction_ == audioalgo::PortDirection::output
            ? audioalgo::PortDirection::input : audioalgo::PortDirection::output;
    }
    [[nodiscard]] const std::optional<Connection>& connection_to_replace() const noexcept {
        return connection_to_replace_;
    }
    [[nodiscard]] const std::string& source_node_id() const noexcept {
        return connection_to_replace_ ? connection_to_replace_->from_node : start_node_id_;
    }
    [[nodiscard]] const std::string& source_port_id() const noexcept {
        return connection_to_replace_ ? connection_to_replace_->from_port : start_port_id_;
    }
    [[nodiscard]] audioalgo::PortDirection preview_source_direction() const noexcept {
        return connection_to_replace_ ? audioalgo::PortDirection::output : start_direction_;
    }
    [[nodiscard]] EditorPoint pointer_position() const noexcept { return pointer_position_; }

private:
    static constexpr float drag_threshold_ = 6.0f;
    std::string start_node_id_;
    std::string start_port_id_;
    audioalgo::PortDirection start_direction_ = audioalgo::PortDirection::output;
    std::optional<Connection> connection_to_replace_;
    bool reconnect_source_endpoint_ = false;
    EditorPoint pointer_position_;
    EditorPoint drag_origin_position_;
    EditorPoint drag_position_;
    bool active_ = false;
    bool dragging_ = false;
};

struct LibraryNodeDropResult {
    std::string node_id;
    EditorPoint position;
};

[[nodiscard]] inline std::optional<EditorPoint> find_library_node_drop_position(
    const ProjectModel& project, const NodeCatalog& registry, const NodeDescriptor& descriptor,
    EditorPoint pointer_position, float view_width, float view_height,
    const CanvasCoordinateTransform& transform = {}) {
    constexpr float left_margin = 16.0f;
    constexpr float top_margin = 42.0f;
    constexpr float right_margin = 16.0f;
    constexpr float bottom_margin = 16.0f;
    constexpr float collision_gap = 8.0f;
    constexpr float search_step = 8.0f;
    constexpr std::size_t candidate_limit = 4096;

    NodeInstance candidate{"drop-candidate", descriptor.type_id, {}, 0.0, 0.0};
    const auto size = make_node_geometry(candidate, &descriptor, transform).bounds;
    const EditorPoint preferred_canvas{pointer_position.x - size.width * 0.5f,
                                       pointer_position.y - size.height * 0.5f};

    const auto is_legal = [&](EditorPoint position) {
        const auto model_position = transform.canvas_to_model(position);
        candidate.x = model_position.x;
        candidate.y = model_position.y;
        const auto bounds = make_node_geometry(candidate, &descriptor, transform).bounds;
        if (bounds.x < left_margin || bounds.y < top_margin ||
            bounds.x + bounds.width > view_width - right_margin ||
            bounds.y + bounds.height > view_height - bottom_margin)
            return false;
        const EditorRect protected_bounds{bounds.x - collision_gap, bounds.y - collision_gap,
                                          bounds.width + collision_gap * 2.0f,
                                          bounds.height + collision_gap * 2.0f};
        return std::none_of(project.nodes.begin(), project.nodes.end(), [&](const auto& existing) {
            return protected_bounds.intersects(
                make_node_geometry(existing, registry.find(existing.type_id), transform).bounds);
        });
    };

    if (is_legal(preferred_canvas))
        return transform.canvas_to_model(preferred_canvas);

    // Search Chebyshev rings on an 8 logical-pixel grid. Rings are considered
    // nearest first, then ties are resolved by ascending squared distance and
    // a stable top/right/bottom/left traversal. At most 4096 positions are read.
    std::size_t checked = 0;
    const auto maximum_radius = std::max(view_width, view_height);
    for (float radius = search_step; radius <= maximum_radius && checked < candidate_limit;
         radius += search_step) {
        std::optional<EditorPoint> best;
        auto best_distance = std::numeric_limits<float>::infinity();
        const auto consider = [&](EditorPoint point) {
            if (checked >= candidate_limit)
                return;
            ++checked;
            if (!is_legal(point))
                return;
            const auto dx = point.x - preferred_canvas.x;
            const auto dy = point.y - preferred_canvas.y;
            const auto distance = dx * dx + dy * dy;
            if (distance < best_distance) {
                best = point;
                best_distance = distance;
            }
        };

        for (float offset = -radius; offset <= radius && checked < candidate_limit; offset += search_step) {
            consider({preferred_canvas.x + offset, preferred_canvas.y - radius});
            consider({preferred_canvas.x + offset, preferred_canvas.y + radius});
        }
        for (float offset = -radius + search_step; offset < radius && checked < candidate_limit;
             offset += search_step) {
            consider({preferred_canvas.x - radius, preferred_canvas.y + offset});
            consider({preferred_canvas.x + radius, preferred_canvas.y + offset});
        }
        if (best)
            return transform.canvas_to_model(*best);
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<LibraryNodeDropResult> try_drop_library_node(
    ProjectModel& project, const NodeCatalog& registry, const std::string& type_id,
    const std::string& node_id, EditorPoint pointer_position, float view_width, float view_height,
    const CanvasCoordinateTransform& transform = {}) {
    const auto* descriptor = registry.find(type_id);
    if (descriptor == nullptr || descriptor->execution_kind == NodeExecutionKind::source)
        return std::nullopt;
    const auto position = find_library_node_drop_position(
        project, registry, *descriptor, pointer_position, view_width, view_height, transform);
    if (!position || !try_add_node_from_descriptor(project, registry, type_id, node_id, position->x, position->y))
        return std::nullopt;
    return LibraryNodeDropResult{node_id, *position};
}

[[nodiscard]] inline std::optional<LibraryNodeDropResult> try_add_source_node_from_drop(
    ProjectModel& project, const NodeCatalog& registry, const std::string& type_id,
    const std::string& node_id, const std::string& resource_path_utf8, EditorPoint pointer_position,
    float view_width, float view_height, std::uint64_t source_epoch, std::uint64_t current_epoch,
    const CanvasCoordinateTransform& transform = {}) {
    const auto* descriptor = registry.find(type_id);
    if (source_epoch != current_epoch || resource_path_utf8.empty() || descriptor == nullptr ||
        descriptor->execution_kind != NodeExecutionKind::source ||
        !can_add_node_of_execution_kind(project, registry, NodeExecutionKind::source))
        return std::nullopt;
    const auto path_parameter = std::find_if(descriptor->parameters.begin(), descriptor->parameters.end(),
        [](const auto& parameter) { return parameter.kind == ParameterKind::path; });
    if (path_parameter == descriptor->parameters.end())
        return std::nullopt;
    const auto position = find_library_node_drop_position(
        project, registry, *descriptor, pointer_position, view_width, view_height, transform);
    auto node = position ? make_node_instance(registry, project.nodes, type_id, node_id,
                                              position->x, position->y) : std::nullopt;
    if (!node)
        return std::nullopt;
    node->parameters[path_parameter->id] = resource_path_utf8;
    project.nodes.push_back(std::move(*node));
    ++project.revision;
    return LibraryNodeDropResult{node_id, *position};
}

} // namespace audioalgo::editor
