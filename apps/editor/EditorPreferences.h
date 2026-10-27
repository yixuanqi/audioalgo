#pragma once
#include "audio_core/Diagnostic.h"
#include <filesystem>
#include <mutex>
#include <optional>
#include <variant>

namespace audioalgo::editor {
struct EditorPreferences {
    std::filesystem::path editor_executable, last_project_directory;
    bool operator==(const EditorPreferences&) const = default;
};
using EditorPreferencesOutcome = std::variant<EditorPreferences, audioalgo::Diagnostic>;
std::filesystem::path editor_preferences_file();
class EditorPreferencesStore {
public:
    explicit EditorPreferencesStore(std::filesystem::path file) : file_(std::move(file)) {}
    EditorPreferencesOutcome load() const;
    // Stage on the message thread; flush the latest value on a worker. All
    // workers serialize before taking the snapshot, so late jobs cannot revert it.
    void stage(EditorPreferences value) const;
    std::optional<audioalgo::Diagnostic> flush() const;
private:
    std::filesystem::path file_;
    mutable std::mutex mutex_;
    mutable std::optional<EditorPreferences> latest_;
};
}
