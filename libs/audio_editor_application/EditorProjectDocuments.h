#pragma once

#include "audio_editor_application/EditorController.h"
#include "audio_editor_application/ProjectDocumentService.h"

#include <cstdint>
#include <functional>

namespace audioalgo {

// Applies persistence results to the authoritative controller only after successful I/O.
class EditorProjectDocuments {
public:
    [[nodiscard]] static ProjectDocumentSaveOutcome save(
        editor::EditorController& controller, const ProjectDocumentService& service,
        const std::filesystem::path& path, const std::function<void()>& after_save = {});

    // Applies a result produced asynchronously from a controller snapshot. A completion for a
    // replaced or edited document must not mark newer work as saved or adopt an old project.
    [[nodiscard]] static ProjectDocumentSaveOutcome complete_save(
        editor::EditorController& controller, ProjectDocumentSaveOutcome outcome,
        const editor::ProjectSessionIdentity& expected_session, std::uint64_t expected_document_generation,
        const std::function<void()>& after_save = {});

    [[nodiscard]] static ProjectDocumentSaveOutcome open(
        editor::EditorController& controller, const ProjectDocumentService& service,
        const std::filesystem::path& path, const std::function<void()>& before_adopt = {});

    [[nodiscard]] static ProjectDocumentSaveOutcome complete_open(
        editor::EditorController& controller, ProjectDocumentLoadOutcome outcome,
        const editor::ProjectSessionIdentity& expected_session, std::uint64_t expected_document_generation,
        const std::function<void()>& before_adopt = {});
};

} // namespace audioalgo
