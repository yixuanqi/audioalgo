#include "audio_core/Utf8LogDecoder.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("PC-04.a log text decoder preserves split UTF8 and replaces malformed or unfinished bytes", "[PC-04.a][pure]") {
    audioalgo::Utf8LogDecoder decoder;
    CHECK(decoder.append("\xe4").empty()); CHECK(decoder.append("\xb8").empty()); CHECK(decoder.append("\xad") == "\xe4\xb8\xad");
    CHECK(decoder.append("\xf0\x9f").empty()); CHECK(decoder.append("\x8e\xb5") == "\xf0\x9f\x8e\xb5");
    CHECK(decoder.append("tail without newline") == "tail without newline"); CHECK(decoder.finish().empty());
    CHECK(decoder.append("\xff") == "\xef\xbf\xbd");
    CHECK(decoder.append("\xc0\x80") == "\xef\xbf\xbd\xef\xbf\xbd");
    CHECK(decoder.append("\xe4").empty()); CHECK(decoder.finish() == "\xef\xbf\xbd");
    CHECK(decoder.append(std::string_view("a\0b", 3)) == "a\xe2\x90\x80" "b");
    for (const auto bytes : {std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"), std::string("\xe0\x80\x80")}) {
        audioalgo::Utf8LogDecoder invalid; const auto text = invalid.append(bytes) + invalid.finish();
        CHECK(text.find("\xef\xbf\xbd") != std::string::npos); CHECK(text != bytes);
    }
}
