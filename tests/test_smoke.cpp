#include <catch2/catch_test_macros.hpp>

TEST_CASE("toolchain builds and runs tests") {
    REQUIRE(sizeof(void*) == 8);
}
