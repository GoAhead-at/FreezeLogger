#include <catch2/catch.hpp>

#include "Watchdog.h"

using FreezeLogger::Watchdog::Action;
using FreezeLogger::Watchdog::Classify;
using FreezeLogger::Watchdog::StalledThread;
using FreezeLogger::Watchdog::State;
using FreezeLogger::Watchdog::Step;

TEST_CASE("Classify: unseeded heartbeats never trip", "[watchdog]") {
    REQUIRE(Classify(/*now=*/100'000, /*main=*/0, /*render=*/0, 5'000)
            == StalledThread::None);
}

TEST_CASE("Classify: only main is stalled", "[watchdog]") {
    const auto now = std::uint64_t{100'000};
    REQUIRE(Classify(now, /*main=*/now - 7'000, /*render=*/now - 100, 5'000)
            == StalledThread::Main);
}

TEST_CASE("Classify: only render is stalled", "[watchdog]") {
    const auto now = std::uint64_t{100'000};
    REQUIRE(Classify(now, /*main=*/now - 100, /*render=*/now - 7'000, 5'000)
            == StalledThread::Render);
}

TEST_CASE("Classify: both stalled together", "[watchdog]") {
    const auto now = std::uint64_t{100'000};
    REQUIRE(Classify(now, /*main=*/now - 6'000, /*render=*/now - 8'000, 5'000)
            == StalledThread::Both);
}

TEST_CASE("Classify: at-threshold is not yet stalled (strict greater-than)", "[watchdog]") {
    const auto now = std::uint64_t{100'000};
    REQUIRE(Classify(now, /*main=*/now - 5'000, /*render=*/now - 5'000, 5'000)
            == StalledThread::None);
}

TEST_CASE("Classify: heartbeat ahead of now is not stalled (clock skew)", "[watchdog]") {
    REQUIRE(Classify(/*now=*/100, /*main=*/200, /*render=*/300, 5'000)
            == StalledThread::None);
}

// =============================================================================
// Step() — full state-machine tests covering cooldown + annotate-on-resolve.
// =============================================================================

namespace {
    constexpr std::uint32_t kThreshold = 5'000;   // ms
    constexpr std::uint32_t kCooldown  = 60;      // s
}

TEST_CASE("Step: unseeded heartbeats produce no action", "[watchdog][step]") {
    State s{};
    auto r = Step(s, /*now=*/100'000, /*main=*/0, /*render=*/0, kThreshold, kCooldown);

    REQUIRE(r.action == Action::None);
    REQUIRE(s.in_freeze == false);
}

TEST_CASE("Step: healthy heartbeats produce no action", "[watchdog][step]") {
    State s{};
    const auto now = std::uint64_t{100'000};
    auto r = Step(s, now, now - 100, now - 100, kThreshold, kCooldown);

    REQUIRE(r.action == Action::None);
    REQUIRE(s.in_freeze == false);
    REQUIRE(r.main_age_ms   == 100);
    REQUIRE(r.render_age_ms == 100);
}

TEST_CASE("Step: stale heartbeat trips EmitSnapshot once", "[watchdog][step]") {
    State s{};
    const auto t0 = std::uint64_t{100'000};
    auto r = Step(s, t0, /*main=*/t0 - 7'000, /*render=*/t0 - 100,
                  kThreshold, kCooldown);

    REQUIRE(r.action == Action::EmitSnapshot);
    REQUIRE(r.stalled == StalledThread::Main);
    REQUIRE(s.in_freeze == true);
    REQUIRE(s.last_snapshot_at_ms == t0);
    REQUIRE(s.freeze_started_at_ms == t0);
}

TEST_CASE("Step: while in_freeze, persisting stall does not re-emit", "[watchdog][step]") {
    State s{};
    const auto t0 = std::uint64_t{100'000};

    // First trip
    Step(s, t0, t0 - 7'000, t0, kThreshold, kCooldown);
    REQUIRE(s.in_freeze == true);

    // 1 second later, still stalled
    auto r = Step(s, t0 + 1'000, /*main=*/t0 - 7'000, /*render=*/t0,
                  kThreshold, kCooldown);

    REQUIRE(r.action == Action::None);  // already in_freeze
    REQUIRE(s.in_freeze == true);
}

TEST_CASE("Step: cooldown suppresses a fresh trip after recovery", "[watchdog][step]") {
    State s{};
    const auto t0 = std::uint64_t{100'000};

    // First trip
    Step(s, t0, t0 - 7'000, t0, kThreshold, kCooldown);

    // Resolve
    auto resolve = Step(s, t0 + 8'000, t0 + 7'500, t0 + 7'500, kThreshold, kCooldown);
    REQUIRE(resolve.action == Action::AnnotateResolve);
    REQUIRE(s.in_freeze == false);

    // 30 s after the snapshot — well within the 60 s cooldown — a NEW stall
    // should be suppressed.
    const auto t1 = t0 + 30'000;
    auto r = Step(s, t1, /*main=*/t1 - 7'000, /*render=*/t1, kThreshold, kCooldown);
    REQUIRE(r.action == Action::None);
    REQUIRE(s.in_freeze == false);
}

TEST_CASE("Step: cooldown lifts after the configured window", "[watchdog][step]") {
    State s{};
    const auto t0 = std::uint64_t{100'000};

    Step(s, t0, t0 - 7'000, t0, kThreshold, kCooldown);          // first trip
    Step(s, t0 + 8'000, t0 + 7'500, t0 + 7'500, kThreshold, kCooldown);  // resolve

    // 70 s after the snapshot — past the 60 s cooldown — a new stall trips.
    const auto t1 = t0 + 70'000;
    auto r = Step(s, t1, /*main=*/t1 - 7'000, /*render=*/t1, kThreshold, kCooldown);
    REQUIRE(r.action == Action::EmitSnapshot);
    REQUIRE(s.in_freeze == true);
}

TEST_CASE("Step: AnnotateResolve carries elapsed-ms", "[watchdog][step]") {
    State s{};
    const auto t0 = std::uint64_t{100'000};

    Step(s, t0, t0 - 7'000, t0, kThreshold, kCooldown);

    const auto t_resolve = t0 + 10'500;
    auto r = Step(s, t_resolve, t_resolve, t_resolve, kThreshold, kCooldown);

    REQUIRE(r.action == Action::AnnotateResolve);
    REQUIRE(r.resolved_after_ms == 10'500);
    REQUIRE(s.in_freeze == false);
    REQUIRE(s.freeze_thread == StalledThread::None);
}

TEST_CASE("Step: classification of dual stall is preserved through state", "[watchdog][step]") {
    State s{};
    const auto t = std::uint64_t{100'000};

    auto r = Step(s, t, /*main=*/t - 7'000, /*render=*/t - 8'000, kThreshold, kCooldown);

    REQUIRE(r.action == Action::EmitSnapshot);
    REQUIRE(r.stalled == StalledThread::Both);
    REQUIRE(s.freeze_thread == StalledThread::Both);
}

// =============================================================================
// Step() — a_canTrip=false (foreground-suppression) path.
// =============================================================================

TEST_CASE("Step: suppression returns SuppressedStall instead of EmitSnapshot", "[watchdog][step][suppress]") {
    State s{};
    const auto t0 = std::uint64_t{100'000};

    auto r = Step(s, t0, /*main=*/t0 - 7'000, /*render=*/t0 - 100,
                  kThreshold, kCooldown, /*a_canTrip=*/false);

    REQUIRE(r.action == Action::SuppressedStall);
    REQUIRE(r.stalled == StalledThread::Main);
    REQUIRE(s.in_freeze == false);          // never entered the snapshot path
    REQUIRE(s.suppressed_freeze == true);   // latched
    REQUIRE(s.last_snapshot_at_ms == 0);    // no cooldown was started
}

TEST_CASE("Step: while suppressed, persisting stall does not re-emit", "[watchdog][step][suppress]") {
    State s{};
    const auto t0 = std::uint64_t{100'000};

    Step(s, t0, t0 - 7'000, t0 - 100, kThreshold, kCooldown, /*a_canTrip=*/false);
    REQUIRE(s.suppressed_freeze == true);

    // 1 second later, still stalled, still suppressed
    auto r = Step(s, t0 + 1'000, /*main=*/t0 - 7'000, /*render=*/t0 - 100,
                  kThreshold, kCooldown, /*a_canTrip=*/false);
    REQUIRE(r.action == Action::None);          // already latched
    REQUIRE(s.suppressed_freeze == true);
}

TEST_CASE("Step: suppressed stall clears silently on resolve (no annotation)", "[watchdog][step][suppress]") {
    State s{};
    const auto t0 = std::uint64_t{100'000};

    Step(s, t0, t0 - 7'000, t0 - 100, kThreshold, kCooldown, /*a_canTrip=*/false);
    REQUIRE(s.suppressed_freeze == true);

    // Heartbeats fresh again -> clear silently. No AnnotateResolve.
    auto r = Step(s, t0 + 5'000, t0 + 5'000, t0 + 5'000,
                  kThreshold, kCooldown, /*a_canTrip=*/false);
    REQUIRE(r.action == Action::None);
    REQUIRE(s.suppressed_freeze == false);
    REQUIRE(s.in_freeze == false);
}

TEST_CASE("Step: focus regained after suppression allows next real stall to trip", "[watchdog][step][suppress]") {
    State s{};
    const auto t0 = std::uint64_t{100'000};

    // Suppressed stall (alt-tab)
    Step(s, t0, t0 - 7'000, t0 - 100, kThreshold, kCooldown, /*a_canTrip=*/false);

    // Heartbeats refresh (focus returned, main resumed)
    Step(s, t0 + 5'000, t0 + 5'000, t0 + 5'000,
         kThreshold, kCooldown, /*a_canTrip=*/true);
    REQUIRE(s.suppressed_freeze == false);

    // 30s later in foreground, a real stall trips normally — and crucially
    // is not gated by cooldown, because no real snapshot was ever written.
    const auto t1 = t0 + 30'000;
    auto r = Step(s, t1, /*main=*/t1 - 7'000, /*render=*/t1,
                  kThreshold, kCooldown, /*a_canTrip=*/true);
    REQUIRE(r.action == Action::EmitSnapshot);
    REQUIRE(s.in_freeze == true);
}

TEST_CASE("Step: backwards-compatible default a_canTrip=true behaves as before", "[watchdog][step][suppress]") {
    State s{};
    const auto t0 = std::uint64_t{100'000};

    // Legacy 6-arg call site — must trip exactly like the existing tests.
    auto r = Step(s, t0, /*main=*/t0 - 7'000, /*render=*/t0 - 100, kThreshold, kCooldown);
    REQUIRE(r.action == Action::EmitSnapshot);
    REQUIRE(s.suppressed_freeze == false);
    REQUIRE(s.in_freeze == true);
}
