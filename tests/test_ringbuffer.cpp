#include <catch2/catch.hpp>

#include "RingBuffer.h"

#include <sstream>

using namespace FreezeLogger;

TEST_CASE("RingBuffer: capacity bound is respected", "[ringbuffer]") {
    RingBuffer::Configure(3, 2);

    for (int i = 0; i < 10; ++i) {
        RingBuffer::Push(RingBuffer::Channel::Papyrus,
                         std::string("p") + std::to_string(i));
    }

    auto snap = RingBuffer::Snapshot();

    int papyrus = 0;
    for (auto& e : snap) {
        if (e.channel == RingBuffer::Channel::Papyrus) ++papyrus;
    }
    REQUIRE(papyrus == 3);
}

TEST_CASE("RingBuffer: chronological order on output", "[ringbuffer]") {
    RingBuffer::Configure(5, 5);

    RingBuffer::Push(RingBuffer::Channel::Papyrus, "first");
    RingBuffer::Push(RingBuffer::Channel::Papyrus, "second");
    RingBuffer::Push(RingBuffer::Channel::Papyrus, "third");

    auto snap = RingBuffer::Snapshot();
    std::vector<std::string> papyrusOrdered;
    for (auto& e : snap) {
        if (e.channel == RingBuffer::Channel::Papyrus) {
            papyrusOrdered.push_back(e.text);
        }
    }
    REQUIRE(papyrusOrdered == std::vector<std::string>{"first", "second", "third"});
}

TEST_CASE("RingBuffer: oldest entry is evicted when full", "[ringbuffer]") {
    RingBuffer::Configure(2, 2);
    RingBuffer::Push(RingBuffer::Channel::Skse, "a");
    RingBuffer::Push(RingBuffer::Channel::Skse, "b");
    RingBuffer::Push(RingBuffer::Channel::Skse, "c");  // evicts "a"

    auto snap = RingBuffer::Snapshot();
    std::vector<std::string> skseOrdered;
    for (auto& e : snap) {
        if (e.channel == RingBuffer::Channel::Skse) {
            skseOrdered.push_back(e.text);
        }
    }
    REQUIRE(skseOrdered == std::vector<std::string>{"b", "c"});
}

TEST_CASE("RingBuffer: WriteSnapshot emits a stable shape", "[ringbuffer]") {
    RingBuffer::Configure(4, 4);
    RingBuffer::Push(RingBuffer::Channel::Papyrus, "papyrus-line");
    RingBuffer::Push(RingBuffer::Channel::Skse,    "skse-event");

    std::ostringstream oss;
    RingBuffer::WriteSnapshot(oss);
    const auto out = oss.str();

    REQUIRE(out.find("Papyrus log")   != std::string::npos);
    REQUIRE(out.find("SKSE messages") != std::string::npos);
    REQUIRE(out.find("papyrus-line")  != std::string::npos);
    REQUIRE(out.find("skse-event")    != std::string::npos);
}
