#pragma once

#include "audio_project/ProjectModel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace audioalgo::editor {

struct EditorPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct CanvasCoordinateTransform {
    [[nodiscard]] EditorPoint event_to_canvas(EditorPoint event_local) const noexcept { return event_local; }
    [[nodiscard]] EditorPoint model_to_canvas(EditorPoint model) const noexcept {
        return {model.x * scale_ + offset_.x, model.y * scale_ + offset_.y};
    }
    [[nodiscard]] EditorPoint canvas_to_model(EditorPoint canvas) const noexcept {
        return {(canvas.x - offset_.x) / scale_, (canvas.y - offset_.y) / scale_};
    }
    void pan_by(EditorPoint delta) noexcept {
        offset_.x += delta.x;
        offset_.y += delta.y;
    }
    void reset_pan() noexcept {
        offset_ = {};
        scale_ = 1.0f;
    }
    [[nodiscard]] EditorPoint pan_offset() const noexcept { return offset_; }
    [[nodiscard]] float zoom_scale() const noexcept { return scale_; }

    // Zooms by a multiplicative factor while keeping the model point under the
    // given view position fixed, so the pointer stays over the same content.
    void zoom_around(EditorPoint view_point, float factor) noexcept {
        constexpr float minimum_zoom = 0.4f;
        constexpr float maximum_zoom = 2.5f;
        const auto next = std::clamp(scale_ * factor, minimum_zoom, maximum_zoom);
        if (next == scale_)
            return;
        const auto applied = next / scale_;
        offset_ = {view_point.x - (view_point.x - offset_.x) * applied,
                   view_point.y - (view_point.y - offset_.y) * applied};
        scale_ = next;
    }

private:
    EditorPoint offset_;
    float scale_ = 1.0f;
};

struct EditorRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] bool intersects(const EditorRect& other) const noexcept {
        return x < other.x + other.width && other.x < x + width &&
               y < other.y + other.height && other.y < y + height;
    }

    [[nodiscard]] bool contains(const EditorPoint& point) const noexcept {
        return point.x >= x && point.y >= y && point.x <= x + width && point.y <= y + height;
    }

    [[nodiscard]] bool contains(const EditorRect& rect) const noexcept {
        return rect.x >= x && rect.y >= y && rect.x + rect.width <= x + width && rect.y + rect.height <= y + height;
    }
};

struct PortVisualGeometry {
    std::string id;
    PortDirection direction = PortDirection::input;
    EditorPoint anchor;
    EditorRect label_bounds;
};

struct NodeVisualGeometry {
    EditorRect bounds;
    EditorRect title_bounds;
    EditorRect summary;
    std::vector<PortVisualGeometry> ports;
};

[[nodiscard]] inline EditorRect model_rect_to_canvas(
    const EditorRect& rect, const CanvasCoordinateTransform& transform) noexcept {
    const auto origin = transform.model_to_canvas({rect.x, rect.y});
    return {origin.x, origin.y, rect.width * transform.zoom_scale(), rect.height * transform.zoom_scale()};
}

[[nodiscard]] inline NodeVisualGeometry make_node_geometry(
    const NodeInstance& node, const NodeDescriptor* descriptor,
    const CanvasCoordinateTransform& transform = {}) {
    constexpr float width = 176.0f;
    constexpr float minimum_height = 100.0f;
    constexpr float first_port_center_y = 64.0f;
    constexpr float port_row_spacing = 20.0f;
    constexpr float port_label_height = 14.0f;
    constexpr float bottom_padding = 10.0f;

    NodeVisualGeometry geometry;
    const auto input_count = descriptor == nullptr ? 0U : static_cast<unsigned>(std::count_if(
        descriptor->ports.begin(), descriptor->ports.end(), [](const auto& port) { return port.direction == PortDirection::input; }));
    const auto output_count = descriptor == nullptr ? 0U : static_cast<unsigned>(std::count_if(
        descriptor->ports.begin(), descriptor->ports.end(), [](const auto& port) { return port.direction == PortDirection::output; }));
    const auto rows = std::max(input_count, output_count);
    const auto height = std::max(minimum_height,
        first_port_center_y + (rows == 0 ? 0.0f : static_cast<float>(rows - 1) * port_row_spacing) +
        port_label_height + bottom_padding);
    // Define every part in model space, then project the complete geometry once.
    const EditorPoint origin{static_cast<float>(node.x), static_cast<float>(node.y)};
    geometry.bounds = {origin.x, origin.y, width, height};
    geometry.title_bounds = {origin.x + 12.0f, origin.y + 7.0f, width - 24.0f, 25.0f};
    geometry.summary = {origin.x + 12.0f, origin.y + 35.0f, width - 24.0f, 18.0f};
    unsigned input_row = 0;
    unsigned output_row = 0;
    if (descriptor != nullptr) {
        for (const auto& port : descriptor->ports) {
            if (port.direction == PortDirection::input) {
                const EditorPoint center{origin.x,
                    origin.y + first_port_center_y + static_cast<float>(input_row++) * port_row_spacing};
                geometry.ports.push_back({port.id, port.direction, center,
                                          {center.x + 12.0f, center.y - port_label_height * 0.5f, 58.0f, port_label_height}});
            } else {
                const EditorPoint center{origin.x + width,
                    origin.y + first_port_center_y + static_cast<float>(output_row++) * port_row_spacing};
                geometry.ports.push_back({port.id, port.direction, center,
                                          {center.x - 70.0f, center.y - port_label_height * 0.5f, 58.0f, port_label_height}});
            }
        }
    }
    geometry.bounds = model_rect_to_canvas(geometry.bounds, transform);
    geometry.title_bounds = model_rect_to_canvas(geometry.title_bounds, transform);
    geometry.summary = model_rect_to_canvas(geometry.summary, transform);
    for (auto& port : geometry.ports) {
        port.anchor = transform.model_to_canvas(port.anchor);
        port.label_bounds = model_rect_to_canvas(port.label_bounds, transform);
    }
    return geometry;
}

[[nodiscard]] inline const NodeInstance* find_topmost_node_at(
    const std::vector<NodeInstance>& nodes, const NodeCatalog& registry, EditorPoint point,
    const CanvasCoordinateTransform& transform = {}, const std::string& ignored_node_id = {}) {
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        if (!ignored_node_id.empty() && it->id == ignored_node_id)
            continue;
        const auto geometry = make_node_geometry(*it, registry.find(it->type_id), transform);
        if (geometry.bounds.contains(point))
            return &*it;
    }
    return nullptr;
}

// Node ids intersecting a canvas-space rectangle, in stable project order.
[[nodiscard]] inline std::vector<std::string> nodes_intersecting_rect(
    const std::vector<NodeInstance>& nodes, const NodeCatalog& registry, const EditorRect& rect,
    const CanvasCoordinateTransform& transform = {}) {
    std::vector<std::string> ids;
    for (const auto& node : nodes) {
        const auto geometry = make_node_geometry(node, registry.find(node.type_id), transform);
        if (rect.intersects(geometry.bounds))
            ids.push_back(node.id);
    }
    return ids;
}

struct PortHit {
    std::string node_id;
    std::string port_id;
    PortDirection direction = PortDirection::input;
};

[[nodiscard]] inline std::optional<PortHit> find_topmost_port_at(
    const std::vector<NodeInstance>& nodes, const NodeCatalog& registry, EditorPoint point,
    const CanvasCoordinateTransform& transform = {}) {
    const auto port_hit_radius = std::max(12.0f, 6.5f * transform.zoom_scale());
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        const auto geometry = make_node_geometry(*it, registry.find(it->type_id), transform);
        const PortVisualGeometry* nearest = nullptr;
        auto nearest_distance = std::numeric_limits<float>::infinity();
        for (const auto& port : geometry.ports) {
            const auto distance = std::hypot(point.x - port.anchor.x, point.y - port.anchor.y);
            if ((distance <= port_hit_radius || port.label_bounds.contains(point)) && distance < nearest_distance) {
                nearest = &port;
                nearest_distance = distance;
            }
        }
        if (nearest != nullptr)
            return PortHit{it->id, nearest->id, nearest->direction};
        // A socket's visible hit area extends past the node rectangle. Continue to
        // lower nodes only when the topmost node does not cover the pointer.
        if (geometry.bounds.contains(point))
            return std::nullopt;
    }
    return std::nullopt;
}

struct CubicConnectionGeometry {
    EditorPoint start;
    EditorPoint control1;
    EditorPoint control2;
    EditorPoint finish;
};

struct NodePositionPreview {
    std::string node_id;
    EditorPoint model_position;
};

[[nodiscard]] inline std::optional<CubicConnectionGeometry> make_connection_geometry(
    const ProjectModel& project, const NodeCatalog& registry, const Connection& connection,
    const CanvasCoordinateTransform& transform = {},
    const std::optional<NodePositionPreview>& position_preview = std::nullopt) {
    const auto source = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
        return node.id == connection.from_node;
    });
    const auto destination = std::find_if(project.nodes.begin(), project.nodes.end(), [&](const auto& node) {
        return node.id == connection.to_node;
    });
    if (source == project.nodes.end() || destination == project.nodes.end())
        return std::nullopt;

    const auto find_anchor = [&](const NodeInstance& node, const std::string& port_id, PortDirection direction)
        -> std::optional<EditorPoint> {
        auto preview_node = node;
        if (position_preview && position_preview->node_id == node.id) {
            preview_node.x = position_preview->model_position.x;
            preview_node.y = position_preview->model_position.y;
        }
        const auto geometry = make_node_geometry(preview_node, registry.find(node.type_id), transform);
        const auto port = std::find_if(geometry.ports.begin(), geometry.ports.end(), [&](const auto& candidate) {
            return candidate.id == port_id && candidate.direction == direction;
        });
        return port == geometry.ports.end() ? std::optional<EditorPoint>{} : port->anchor;
    };
    const auto start = find_anchor(*source, connection.from_port, PortDirection::output);
    const auto finish = find_anchor(*destination, connection.to_port, PortDirection::input);
    if (!start || !finish)
        return std::nullopt;

    const auto control_distance = std::max(48.0f * transform.zoom_scale(), (finish->x - start->x) * 0.44f);
    return CubicConnectionGeometry{*start, {start->x + control_distance, start->y},
                                   {finish->x - control_distance, finish->y}, *finish};
}

[[nodiscard]] inline EditorPoint midpoint(EditorPoint first, EditorPoint second) noexcept {
    return {(first.x + second.x) * 0.5f, (first.y + second.y) * 0.5f};
}

[[nodiscard]] inline float distance_to_segment(EditorPoint point, EditorPoint start, EditorPoint finish) noexcept {
    const auto dx = finish.x - start.x;
    const auto dy = finish.y - start.y;
    const auto length_squared = dx * dx + dy * dy;
    if (length_squared <= 0.000001f)
        return std::hypot(point.x - start.x, point.y - start.y);
    const auto projection = std::clamp(((point.x - start.x) * dx + (point.y - start.y) * dy) / length_squared,
                                       0.0f, 1.0f);
    return std::hypot(point.x - (start.x + projection * dx), point.y - (start.y + projection * dy));
}

inline void flatten_connection_curve(EditorPoint start, EditorPoint control1, EditorPoint control2, EditorPoint finish,
                                     std::vector<EditorPoint>& points, unsigned depth = 0) {
    constexpr float flatten_tolerance = 0.05f;
    constexpr unsigned max_depth = 14;
    const auto flatness = std::max(distance_to_segment(control1, start, finish),
                                   distance_to_segment(control2, start, finish));
    if (flatness <= flatten_tolerance || depth >= max_depth) {
        points.push_back(finish);
        return;
    }

    const auto start_control = midpoint(start, control1);
    const auto control_mid = midpoint(control1, control2);
    const auto end_control = midpoint(control2, finish);
    const auto left_mid = midpoint(start_control, control_mid);
    const auto right_mid = midpoint(control_mid, end_control);
    const auto curve_mid = midpoint(left_mid, right_mid);
    flatten_connection_curve(start, start_control, left_mid, curve_mid, points, depth + 1);
    flatten_connection_curve(curve_mid, right_mid, end_control, finish, points, depth + 1);
}

[[nodiscard]] inline float distance_to_connection(const CubicConnectionGeometry& curve, EditorPoint point) {
    std::vector<EditorPoint> flattened{curve.start};
    flatten_connection_curve(curve.start, curve.control1, curve.control2, curve.finish, flattened);
    auto minimum = std::numeric_limits<float>::infinity();
    for (std::size_t index = 1; index < flattened.size(); ++index)
        minimum = std::min(minimum, distance_to_segment(point, flattened[index - 1], flattened[index]));
    return minimum;
}

[[nodiscard]] inline std::optional<std::size_t> find_visible_connection_at(
    const ProjectModel& project, const NodeCatalog& registry, EditorPoint point, float tolerance = 8.0f,
    const CanvasCoordinateTransform& transform = {}, const std::string& ignored_node_id = {}) {
    if (tolerance < 0.0f || find_topmost_node_at(project.nodes, registry, point, transform, ignored_node_id) != nullptr)
        return std::nullopt;
    std::optional<std::size_t> nearest_index;
    auto nearest_distance = std::numeric_limits<float>::infinity();
    // Scan in stable project order. Strictly smaller distances replace the
    // current result, so exact ties resolve to the lowest connection index.
    for (std::size_t index = 0; index < project.connections.size(); ++index) {
        const auto geometry = make_connection_geometry(project, registry, project.connections[index], transform);
        if (!geometry)
            continue;
        const auto distance = distance_to_connection(*geometry, point);
        if (distance <= tolerance && distance < nearest_distance) {
            nearest_index = index;
            nearest_distance = distance;
        }
    }
    return nearest_index;
}

[[nodiscard]] inline std::optional<EditorPoint> find_node_placement(
    const std::vector<NodeInstance>& existing_nodes, const NodeCatalog& registry,
    const NodeDescriptor& descriptor, float view_width, float view_height,
    const CanvasCoordinateTransform& transform = {}) {
    constexpr float left_margin = 16.0f;
    constexpr float top_margin = 42.0f;
    constexpr float right_margin = 16.0f;
    constexpr float bottom_margin = 16.0f;
    constexpr float horizontal_gap = 24.0f;
    constexpr float vertical_gap = 24.0f;

    NodeInstance candidate{"placement-candidate", descriptor.type_id, {}, 0.0, 0.0};
    const auto size = make_node_geometry(candidate, &descriptor, transform).bounds;
    const auto last_x = view_width - right_margin - size.width;
    const auto last_y = view_height - bottom_margin - size.height;
    if (last_x < left_margin || last_y < top_margin)
        return std::nullopt;

    for (float y = top_margin; y <= last_y; y += size.height + vertical_gap) {
        for (float x = left_margin; x <= last_x; x += size.width + horizontal_gap) {
            const auto model_position = transform.canvas_to_model({x, y});
            candidate.x = model_position.x;
            candidate.y = model_position.y;
            const auto candidate_bounds = make_node_geometry(candidate, &descriptor, transform).bounds;
            const bool overlaps = std::any_of(existing_nodes.begin(), existing_nodes.end(), [&](const auto& existing) {
                return candidate_bounds.intersects(
                    make_node_geometry(existing, registry.find(existing.type_id), transform).bounds);
            });
            if (!overlaps)
                return model_position;
        }
    }
    return std::nullopt;
}

} // namespace audioalgo::editor
