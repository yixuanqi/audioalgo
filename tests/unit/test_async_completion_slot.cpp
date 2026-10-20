#include "audio_editor_application/AsyncCompletionSlot.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

namespace {
struct ChooserState {
    std::string project_id = "project-B";
    std::string selected_path = "B-existing.aaproj";
    std::string status = "B-ready";
    bool run_after_save = false;
    bool operator==(const ChooserState&) const = default;
};
} // namespace

TEST_CASE("AR-09.c A stale chooser result cannot consume the newer completion",
          "[R2][AR-09.c][pure][lifecycle]") {
    audioalgo::AsyncCompletionSlot<std::string> completions;
    ChooserState state;

    const auto chooser_a = completions.begin([&](std::string selected_path) {
        state.selected_path = std::move(selected_path);
        state.project_id = "project-A";
        state.status = "A-loaded";
    });

    // New/Open revokes chooser A before its already-queued callback is delivered.
    completions.invalidate();
    const auto chooser_b = completions.begin([&](std::string selected_path) {
        state.selected_path = std::move(selected_path);
        state.project_id = state.selected_path == "B-selected.aaproj" ? "project-B" : "project-A";
        state.status = "B-selection-loaded";
        state.run_after_save = true;
    });
    const auto before_stale_delivery = state;

    auto stale_completion = completions.take(chooser_a);
    if (stale_completion)
        stale_completion("A-selected.aaproj");

    CHECK_FALSE(stale_completion);
    CHECK(state == before_stale_delivery);

    auto current_completion = completions.take(chooser_b);
    REQUIRE(current_completion);
    current_completion("B-selected.aaproj");
    CHECK(state.project_id == "project-B");
    CHECK(state.selected_path == "B-selected.aaproj");
    CHECK(state.status == "B-selection-loaded");
    CHECK(state.run_after_save);
    CHECK_FALSE(completions.take(chooser_b));
}

TEST_CASE("AR-09.c a chooser callback after view shutdown is discarded",
          "[R2][AR-09.c][pure][lifecycle]") {
    audioalgo::AsyncCompletionSlot<std::string> closing_view;
    std::string visible_status = "closing";
    const auto pending_chooser = closing_view.begin([&](std::string selected_path) {
        visible_status = std::move(selected_path);
    });

    closing_view.close();
    auto late_chooser_result = closing_view.take(pending_chooser);
    if (late_chooser_result)
        late_chooser_result("selected-after-close.aaproj");

    CHECK_FALSE(late_chooser_result);
    CHECK(visible_status == "closing");
    CHECK(closing_view.begin([&](std::string) { visible_status = "reopened"; }) == 0);
}

TEST_CASE("AR-09.c cancellation and reopen keep chooser completions isolated",
          "[R2][AR-09.c][pure][lifecycle]") {
    audioalgo::AsyncCompletionSlot<std::string> old_view;
    std::string old_view_result = "not completed";
    const auto cancelled_chooser = old_view.begin([&](std::string selected_path) {
        old_view_result = std::move(selected_path);
    });
    auto cancellation = old_view.take(cancelled_chooser);
    REQUIRE(cancellation);
    cancellation({});
    CHECK(old_view_result.empty());
    CHECK_FALSE(old_view.take(cancelled_chooser));

    old_view_result = "closing";
    const auto late_chooser = old_view.begin([&](std::string selected_path) {
        old_view_result = std::move(selected_path);
    });
    old_view.close();
    auto late_chooser_result = old_view.take(late_chooser);
    if (late_chooser_result)
        late_chooser_result("old-view-result.aaproj");
    CHECK_FALSE(late_chooser_result);
    CHECK(old_view_result == "closing");

    audioalgo::AsyncCompletionSlot<std::string> reopened_view;
    std::string reopened_view_result = "ready";
    const auto reopened_chooser = reopened_view.begin([&](std::string selected_path) {
        reopened_view_result = std::move(selected_path);
    });
    auto current_chooser_result = reopened_view.take(reopened_chooser);
    REQUIRE(current_chooser_result);
    current_chooser_result("new-view-project.aaproj");
    CHECK(reopened_view_result == "new-view-project.aaproj");
}
