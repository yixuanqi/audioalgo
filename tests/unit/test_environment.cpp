#include <catch2/catch_test_macros.hpp>

TEST_CASE("BLD-04 Catch2 runner is active", "[G0][BLD-04]") {
#if defined(_MSVC_LANG)
    REQUIRE(_MSVC_LANG >= 202002L);
#else
    REQUIRE(__cplusplus >= 202002L);
#endif
    SUCCEED("The registered test executable is using C++20.");
}
