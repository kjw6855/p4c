#include "backends/p4tools/modules/symbex/lib/deadline.h"

#include <atomic>
#include <chrono>

namespace P4::P4Tools::Symbex {

namespace {

using SteadyClock = std::chrono::steady_clock;

/// t0 of the run, set by Deadline::start(). Default-constructed until then, which only affects
/// elapsedSeconds() on the (impossible) path where start() was never called.
SteadyClock::time_point startPoint;

/// t0 + budget. Only meaningful when deadlineArmed.
SteadyClock::time_point deadlinePoint;

/// p4symbex is single-threaded -- the --jobs parallelism in the Python harness is one process per
/// program -- so these need not be atomic today. They are anyway, so that adding a worker thread
/// later cannot silently turn this file into a data race.
std::atomic<bool> deadlineArmed{false};
std::atomic<bool> deadlineHit{false};

}  // namespace

void Deadline::start() { startPoint = SteadyClock::now(); }

void Deadline::arm(uint64_t seconds) {
    if (seconds == 0) {
        return;
    }
    deadlinePoint = startPoint + std::chrono::seconds(seconds);
    deadlineArmed = true;
}

bool Deadline::armed() { return deadlineArmed; }

bool Deadline::expired() { return deadlineArmed && SteadyClock::now() >= deadlinePoint; }

bool Deadline::checkAndLatch() {
    if (!expired()) {
        return false;
    }
    markHit();
    return true;
}

void Deadline::markHit() { deadlineHit = true; }

bool Deadline::wasHit() { return deadlineHit; }

uint64_t Deadline::elapsedSeconds() {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(SteadyClock::now() - startPoint);
    return static_cast<uint64_t>(elapsed.count());
}

}  // namespace P4::P4Tools::Symbex
