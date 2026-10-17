#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace audioalgo::editor {

enum class EditorShortcutAction { none, undo, redo };

[[nodiscard]] inline EditorShortcutAction editor_shortcut_action(const juce::KeyPress& key) noexcept {
    const auto modifiers = key.getModifiers();
    if ((!modifiers.isCtrlDown() && !modifiers.isCommandDown()) || modifiers.isAltDown())
        return EditorShortcutAction::none;

    const auto key_code = key.getKeyCode();
    if (key_code == 'z' || key_code == 'Z')
        return modifiers.isShiftDown() ? EditorShortcutAction::redo : EditorShortcutAction::undo;
    if ((key_code == 'y' || key_code == 'Y') && !modifiers.isShiftDown())
        return EditorShortcutAction::redo;
    return EditorShortcutAction::none;
}

} // namespace audioalgo::editor
