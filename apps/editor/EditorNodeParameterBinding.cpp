#include "EditorNodeParameterBinding.h"

#include <utility>

namespace audioalgo::editor {

EditorNodeParameterBinding::EditorNodeParameterBinding(EditorController& controller,
                                                       const NodeViewSnapshot& snapshot)
    : controller_(controller), session_(snapshot.session), node_id_(snapshot.node.id) {}

bool EditorNodeParameterBinding::matches(const std::optional<NodeViewSnapshot>& current) const noexcept {
    return current.has_value() && current->session == session_ && current->node.id == node_id_;
}

std::optional<ParameterEditOutcome> EditorNodeParameterBinding::set_parameter_if_current(
    const std::optional<NodeViewSnapshot>& current,
    std::string parameter_id,
    ParameterValue value) const {
    if (!matches(current))
        return std::nullopt;
    return controller_.set_parameter(session_, node_id_, parameter_id, std::move(value));
}

} // namespace audioalgo::editor
