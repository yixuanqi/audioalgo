#pragma once

#include "audio_editor_application/EditorController.h"

#include <optional>
#include <string>

namespace audioalgo::editor {

// Captures the project session and node represented by one rendered parameter panel.
// UI callbacks must present the current snapshot before they can edit that node.
class EditorNodeParameterBinding final {
public:
    EditorNodeParameterBinding(EditorController& controller, const NodeViewSnapshot& snapshot);

    [[nodiscard]] bool matches(const std::optional<NodeViewSnapshot>& current) const noexcept;
    [[nodiscard]] const ProjectSessionIdentity& session() const noexcept { return session_; }
    [[nodiscard]] const std::string& node_id() const noexcept { return node_id_; }

    [[nodiscard]] std::optional<ParameterEditOutcome> set_parameter_if_current(
        const std::optional<NodeViewSnapshot>& current,
        std::string parameter_id,
        ParameterValue value) const;

private:
    EditorController& controller_;
    ProjectSessionIdentity session_;
    std::string node_id_;
};

} // namespace audioalgo::editor
