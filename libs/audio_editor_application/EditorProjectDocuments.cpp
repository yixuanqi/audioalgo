#include "audio_editor_application/EditorProjectDocuments.h"

#include <utility>

namespace audioalgo {
namespace {

Diagnostic stale_document_result() {
    return {"project_session_stale", "The project changed while the document operation was running"};
}

bool still_matches(const editor::EditorController& controller,
                   const editor::ProjectSessionIdentity& expected_session,
                   std::uint64_t expected_document_generation) {
    return controller.session_identity() == expected_session &&
           controller.document_generation() == expected_document_generation;
}

} // namespace

ProjectDocumentSaveOutcome EditorProjectDocuments::save(
    editor::EditorController& controller, const ProjectDocumentService& service,
    const std::filesystem::path& path, const std::function<void()>& after_save) {
    const auto expected_session = controller.session_identity();
    const auto expected_generation = controller.document_generation();
    auto outcome = service.save(path, controller.project());
    return complete_save(controller, std::move(outcome), expected_session, expected_generation, after_save);
}

ProjectDocumentSaveOutcome EditorProjectDocuments::complete_save(
    editor::EditorController& controller, ProjectDocumentSaveOutcome outcome,
    const editor::ProjectSessionIdentity& expected_session, std::uint64_t expected_document_generation,
    const std::function<void()>& after_save) {
    if (!still_matches(controller, expected_session, expected_document_generation))
        return stale_document_result();
    if (const auto* error = std::get_if<Diagnostic>(&outcome))
        return *error;
    if (after_save)
        after_save();
    controller.mark_saved();
    return std::monostate{};
}

ProjectDocumentSaveOutcome EditorProjectDocuments::open(
    editor::EditorController& controller, const ProjectDocumentService& service,
    const std::filesystem::path& path, const std::function<void()>& before_adopt) {
    const auto expected_session = controller.session_identity();
    const auto expected_generation = controller.document_generation();
    auto outcome = service.load(path);
    return complete_open(controller, std::move(outcome), expected_session, expected_generation, before_adopt);
}

ProjectDocumentSaveOutcome EditorProjectDocuments::complete_open(
    editor::EditorController& controller, ProjectDocumentLoadOutcome outcome,
    const editor::ProjectSessionIdentity& expected_session, std::uint64_t expected_document_generation,
    const std::function<void()>& before_adopt) {
    if (!still_matches(controller, expected_session, expected_document_generation))
        return stale_document_result();
    if (const auto* error = std::get_if<Diagnostic>(&outcome))
        return *error;
    if (before_adopt)
        before_adopt();
    controller.adopt_project(std::move(std::get<ProjectModel>(outcome)));
    return std::monostate{};
}

} // namespace audioalgo
