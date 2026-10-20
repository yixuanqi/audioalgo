#include "audio_editor_application/RunReception.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {
struct Display {
    std::string result = "B-result";
    std::string waveform = "B-waveform";
    std::string playback = "B.wav";
    std::string status = "B-ready";
    bool operator==(const Display&) const = default;
};
}

TEST_CASE("AR-03.a", "[architecture][pure][run_identity]") {
    audioalgo::RunReception reception;
    const audioalgo::RunIdentity a{"project-A", 1, "request-A", 7};
    reception.begin(a);
    // Open B while A is held by the controlled service. Revision is identical.
    reception.invalidate();
    Display b;
    const auto before = b;
    const auto request_before = reception.active();
    if (reception.accepts(a, "project-B", 2, 7))
        b = {"A-result", "A-waveform", "A.wav", "A-complete"};
    REQUIRE(b.result == before.result);
    REQUIRE(b.waveform == before.waveform);
    REQUIRE(b.playback == before.playback);
    REQUIRE(b.status == before.status);
    REQUIRE(reception.active() == request_before);
}

TEST_CASE("AR-03.b", "[architecture][pure][run_identity]") {
    audioalgo::RunReception reception;
    const audioalgo::RunIdentity a{"project-A", 1, "request-A", 7};
    reception.begin(a);
    reception.invalidate();
    Display b;
    const auto before = b;
    // Progress, error and cancellation share the same guarded message-thread entry.
    for (const auto* event_kind : {"progress", "error", "cancelled"}) {
        INFO(event_kind);
        if (reception.accepts(a, "project-B", 2, 7))
            b = {"A-result", "A-waveform", "A.wav", event_kind};
    }
    REQUIRE(b == before);
}

TEST_CASE("AR-03.c", "[architecture][pure][run_identity]") {
    audioalgo::RunReception reception;
    const audioalgo::RunIdentity first{"project-A", 1, "request-first", 7};
    const audioalgo::RunIdentity latest{"project-A", 1, "request-latest", 7};
    reception.begin(first);
    reception.begin(latest);
    REQUIRE_FALSE(reception.accepts(first, "project-A", 1, 7));
    REQUIRE(reception.accepts(latest, "project-A", 1, 7));
}

TEST_CASE("AR-03.d", "[architecture][pure][run_identity]") {
    audioalgo::RunReception reception;
    const audioalgo::RunIdentity before_open{"same-project", 1, "old-request", 7};
    reception.begin(before_open);
    reception.invalidate();
    const audioalgo::RunIdentity reopened{"same-project", 2, "new-request", 7};
    reception.begin(reopened);
    REQUIRE_FALSE(reception.accepts(before_open, "same-project", 2, 7));
    REQUIRE(reception.accepts(reopened, "same-project", 2, 7));
}

TEST_CASE("AR-03.e", "[architecture][pure][run_identity]") {
    audioalgo::RunReception reception;
    const audioalgo::RunIdentity old{"project-A", 1, "request-old", 7};
    reception.begin(old);
    for (const auto revision : {8ULL, 9ULL, 10ULL}) { // parameter, topology, then undo/redo revisions
        REQUIRE_FALSE(reception.accepts(old, "project-A", 1, revision));
    }
}

TEST_CASE("AR-03.f", "[architecture][pure][run_identity]") {
    audioalgo::RunReception reception;
    const audioalgo::RunIdentity current{"project-A", 1, "request-current", 7};
    const audioalgo::RunIdentity historical{"project-A", 1, "request-historical", 6};
    reception.begin(current);
    REQUIRE(reception.accepts(current, "project-A", 1, 7));
    REQUIRE_FALSE(reception.accepts(historical, "project-A", 1, 7));
}
