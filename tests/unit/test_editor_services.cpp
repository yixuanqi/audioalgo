#include "audio_editor_application/AudioPreviewService.h"
#include "audio_editor_application/EditorController.h"
#include "audio_editor_application/EditorProjectDocuments.h"
#include "audio_editor_application/ProjectDocumentService.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <utility>
#include <variant>

namespace {

class ProjectDocumentProbe final : public audioalgo::ProjectDocumentService {
public:
    audioalgo::ProjectDocumentLoadOutcome load(const std::filesystem::path& path) const override {
        last_loaded_path = path;
        return load_outcome;
    }

    audioalgo::ProjectDocumentSaveOutcome save(const std::filesystem::path& path,
                                               const audioalgo::ProjectModel& project) const override {
        last_saved_path = path;
        saved_project = project;
        return save_outcome;
    }

    mutable std::filesystem::path last_loaded_path;
    mutable std::filesystem::path last_saved_path;
    mutable audioalgo::ProjectModel saved_project;
    audioalgo::ProjectDocumentLoadOutcome load_outcome = audioalgo::Diagnostic{"load_not_configured", ""};
    audioalgo::ProjectDocumentSaveOutcome save_outcome = std::monostate{};
};

class PreviewProbe final : public audioalgo::AudioPreviewService {
public:
    audioalgo::AudioPreviewOutcome play(const std::filesystem::path& audio_file) override {
        current_file = audio_file;
        playing = true;
        return std::monostate{};
    }
    void pause() override { playing = false; }
    void stop() override {
        playing = false;
        current_file.clear();
        position = 0.0;
    }
    void shutdown() override {
        stop();
        shut_down = true;
    }
    bool is_playing() const noexcept override { return playing; }
    double position_seconds() const noexcept override { return position; }

    std::filesystem::path current_file;
    double position = 0.0;
    bool playing = false;
    bool shut_down = false;
};

} // namespace

TEST_CASE("AR-09.a injected document service opens only successful projects after host invalidation",
          "[R2][AR-09.a][pure]") {
    audioalgo::NodeRegistry registry;
    audioalgo::editor::EditorController controller(registry);
    audioalgo::ProjectModel project_a;
    project_a.project_id = "project-A";
    project_a.name = "Project A";
    controller.adopt_project(project_a);
    REQUIRE(controller.set_project_name("Project A edited").status == audioalgo::editor::ProjectEditStatus::applied);

    audioalgo::ProjectModel project_b;
    project_b.project_id = "project-B";
    project_b.name = "Project B";
    ProjectDocumentProbe documents;
    documents.load_outcome = project_b;
    bool invalidated_before_adopt = false;
    const auto opened = audioalgo::EditorProjectDocuments::open(
        controller, documents, "project-b.aaproj", [&] {
            invalidated_before_adopt = true;
            CHECK(controller.project().project_id == "project-A");
            CHECK(controller.dirty());
        });

    REQUIRE(std::holds_alternative<std::monostate>(opened));
    CHECK(documents.last_loaded_path == std::filesystem::path{"project-b.aaproj"});
    CHECK(invalidated_before_adopt);
    CHECK(controller.project().project_id == "project-B");
    CHECK(controller.project().name == "Project B");
    CHECK_FALSE(controller.dirty());
    CHECK_FALSE(controller.results_stale());
    CHECK(controller.history().undo_size() == 0);

    const auto before_failure = controller.project();
    documents.load_outcome = audioalgo::Diagnostic{"project_decode_failed", "invalid fixture"};
    bool failure_invalidated = false;
    const auto failed = audioalgo::EditorProjectDocuments::open(
        controller, documents, "bad.aaproj", [&] { failure_invalidated = true; });
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(failed));
    CHECK(std::get<audioalgo::Diagnostic>(failed).code == "project_decode_failed");
    CHECK_FALSE(failure_invalidated);
    CHECK(controller.project().project_id == before_failure.project_id);
    CHECK(controller.project().name == before_failure.name);
}

TEST_CASE("AR-09.a injected document service clears dirty only after a successful save",
          "[R2][AR-09.a][pure]") {
    audioalgo::NodeRegistry registry;
    audioalgo::editor::EditorController controller(registry);
    audioalgo::ProjectModel project;
    project.project_id = "save-project";
    project.name = "Before save";
    controller.adopt_project(project);
    REQUIRE(controller.set_project_name("Saved name").status == audioalgo::editor::ProjectEditStatus::applied);

    ProjectDocumentProbe documents;
    const auto saved = audioalgo::EditorProjectDocuments::save(controller, documents, "saved.aaproj");
    REQUIRE(std::holds_alternative<std::monostate>(saved));
    CHECK(documents.last_saved_path == std::filesystem::path{"saved.aaproj"});
    CHECK(documents.saved_project.project_id == "save-project");
    CHECK(documents.saved_project.name == "Saved name");
    CHECK_FALSE(controller.dirty());

    REQUIRE(controller.set_project_name("Unsaved again").status == audioalgo::editor::ProjectEditStatus::applied);
    documents.save_outcome = audioalgo::Diagnostic{"project_write_failed", "disk unavailable"};
    const auto failed = audioalgo::EditorProjectDocuments::save(controller, documents, "failed.aaproj");
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(failed));
    CHECK(std::get<audioalgo::Diagnostic>(failed).code == "project_write_failed");
    CHECK(controller.project().name == "Unsaved again");
    CHECK(controller.dirty());
}

TEST_CASE("AR-09.a preview service is replaceable without exposing a device backend",
          "[R2][AR-09.a][pure]") {
    PreviewProbe preview;
    const auto started = preview.play("rendered output.wav");
    REQUIRE(std::holds_alternative<std::monostate>(started));
    CHECK(preview.is_playing());
    CHECK(preview.current_file == std::filesystem::path{"rendered output.wav"});
    preview.pause();
    CHECK_FALSE(preview.is_playing());
    preview.position = 2.5;
    CHECK(preview.position_seconds() == 2.5);
    preview.stop();
    CHECK_FALSE(preview.is_playing());
    CHECK(preview.current_file.empty());
    CHECK(preview.position_seconds() == 0.0);
    preview.shutdown();
    CHECK(preview.shut_down);
}

TEST_CASE("AR-09.e asynchronous document completions reject edited and replaced snapshots",
          "[R2][AR-09.e][pure]") {
    audioalgo::NodeRegistry registry;
    audioalgo::editor::EditorController controller(registry);
    audioalgo::ProjectModel project_a;
    project_a.project_id = "async-project-A";
    project_a.name = "Project A";
    controller.adopt_project(project_a);

    const auto save_session = controller.session_identity();
    const auto save_generation = controller.document_generation();
    REQUIRE(controller.set_project_name("Changed while save was running").status ==
            audioalgo::editor::ProjectEditStatus::applied);
    bool applied_save_path = false;
    const auto stale_save = audioalgo::EditorProjectDocuments::complete_save(
        controller, std::monostate{}, save_session, save_generation, [&] { applied_save_path = true; });
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(stale_save));
    CHECK(std::get<audioalgo::Diagnostic>(stale_save).code == "project_session_stale");
    CHECK_FALSE(applied_save_path);
    CHECK(controller.dirty());
    CHECK(controller.project().name == "Changed while save was running");

    const auto open_session = controller.session_identity();
    const auto open_generation = controller.document_generation();
    audioalgo::ProjectModel project_b;
    project_b.project_id = "async-project-B";
    project_b.name = "Project B";
    controller.adopt_project(project_b);
    bool invalidated_for_stale_open = false;
    audioalgo::ProjectModel stale_loaded_project;
    stale_loaded_project.project_id = "stale-loaded-project";
    const auto stale_open = audioalgo::EditorProjectDocuments::complete_open(
        controller, stale_loaded_project, open_session, open_generation,
        [&] { invalidated_for_stale_open = true; });
    REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(stale_open));
    CHECK(std::get<audioalgo::Diagnostic>(stale_open).code == "project_session_stale");
    CHECK_FALSE(invalidated_for_stale_open);
    CHECK(controller.project().project_id == "async-project-B");
}
