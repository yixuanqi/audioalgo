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
    ComponentBounds library_pane;
    ComponentBounds library_splitter;
    ComponentBounds inspector_splitter;
    ComponentBounds results_splitter;
};

struct EditorPanelSizes { int library = 164, inspector = 270, results = 168; };

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

inline EditorComponentLayout make_editor_component_layout(int width, int height, EditorPanelSizes sizes = {}) noexcept {
    constexpr int rail = 8;
    const int left = std::clamp(sizes.library, 148, std::max(148, width - 220 - 240 - 2 * rail));
    const int right = std::clamp(sizes.inspector, 220, std::max(220, width - left - 240 - 2 * rail));
    constexpr int top = 54;
    const int waveform_height = std::clamp(sizes.results, 128, std::max(128, height - top - 160 - 30 - rail));
    constexpr int status_height = 30;

    const int library_height = std::max(48, height - 164);
    const int canvas_width = std::max(180, width - left - right - 2 * rail);
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
        {width - right + 12, height - status_height - 40, right - 24, 30},
        {left + rail, top, canvas_width, canvas_height},
        {width - right, top, right, height - top - status_height - 44},
        {left + rail, top + canvas_height + rail, canvas_width, waveform_height},
        {left + rail + 10, height - status_height, canvas_width - 20, status_height - 2},
        {0, top, left, height - top},
        {left, top, rail, height - top - status_height},
        {width - right - rail, top, rail, height - top - status_height},
        {left + rail, top + canvas_height, canvas_width, rail},
    };
}

} // namespace audioalgo::editor
