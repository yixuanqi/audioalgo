#pragma once

#include <algorithm>

namespace audioalgo::editor {

struct ComponentBounds {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct EditorComponentLayout {
    ComponentBounds new_button;
    ComponentBounds open_button;
    ComponentBounds save_button;
    ComponentBounds undo_button;
    ComponentBounds redo_button;
    ComponentBounds run_button;
    ComponentBounds play_button;
    ComponentBounds library_title;
    ComponentBounds library_viewport;
    ComponentBounds delete_button;
    ComponentBounds canvas;
    ComponentBounds inspector;
    ComponentBounds waveform;
    ComponentBounds status;
};

struct EditorWindowSize {
    int width = 0;
    int height = 0;
};

inline constexpr EditorWindowSize default_editor_window_size{1280, 800};

inline EditorWindowSize make_initial_editor_window_size(int work_area_width, int work_area_height) noexcept {
    constexpr int work_area_margin = 24;
    return {
        std::min(default_editor_window_size.width, std::max(1, work_area_width - work_area_margin)),
        std::min(default_editor_window_size.height, std::max(1, work_area_height - work_area_margin)),
    };
}

inline EditorComponentLayout make_editor_component_layout(int width, int height) noexcept {
    constexpr int left = 164;
    constexpr int right = 270;
    constexpr int top = 54;
    constexpr int waveform_height = 168;
    constexpr int status_height = 30;

    const int library_height = std::max(48, height - 164);
    const int canvas_width = std::max(180, width - left - right - 14);
    const int canvas_height = std::max(160, height - top - waveform_height - status_height - 8);

    return {
        {178, 10, 60, 34},
        {244, 10, 66, 34},
        {316, 10, 66, 34},
        {388, 10, 60, 34},
        {454, 10, 60, 34},
        {530, 10, 82, 34},
        {618, 10, 98, 34},
        {18, 78, 130, 22},
        {10, 108, 144, library_height},
        {14, std::max(112, height - 46), 136, 34},
        {left, top, canvas_width, canvas_height},
        {width - right, top, right, canvas_height + waveform_height + 4},
        {left, top + canvas_height + 8, canvas_width, waveform_height},
        {left + 10, height - status_height, canvas_width - 20, status_height - 2},
    };
}

} // namespace audioalgo::editor
