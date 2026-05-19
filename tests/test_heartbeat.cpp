#include <catch2/catch.hpp>

#include "Heartbeat.h"

using namespace FreezeLogger;

TEST_CASE("Heartbeat: initial state is zero", "[heartbeat]") {
    Heartbeat::test::Reset();
    REQUIRE(Heartbeat::Main()   == 0);
    REQUIRE(Heartbeat::Render() == 0);
}

TEST_CASE("Heartbeat: TickMain advances main but not render", "[heartbeat]") {
    Heartbeat::test::Reset();
    Heartbeat::TickMain();
    REQUIRE(Heartbeat::Main()   != 0);
    REQUIRE(Heartbeat::Render() == 0);
}

TEST_CASE("Heartbeat: TickRender advances render but not main", "[heartbeat]") {
    Heartbeat::test::Reset();
    Heartbeat::TickRender();
    REQUIRE(Heartbeat::Render() != 0);
    REQUIRE(Heartbeat::Main()   == 0);
}

TEST_CASE("Heartbeat: test seam can set explicit values", "[heartbeat]") {
    Heartbeat::test::Reset();
    Heartbeat::test::SetMain(123456);
    Heartbeat::test::SetRender(654321);
    REQUIRE(Heartbeat::Main()   == 123456);
    REQUIRE(Heartbeat::Render() == 654321);
}
