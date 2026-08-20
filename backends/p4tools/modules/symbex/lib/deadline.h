#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_DEADLINE_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_DEADLINE_H_

#include <cstdint>

namespace P4::P4Tools::Symbex {

/// Wall-clock budget for one p4symbex run, set by --timeout.
///
/// Cooperative rather than signal-based: the search polls checkAndLatch() at points where
/// abandoning work is safe, so the process returns normally and still flushes its summary line and
/// its --print-performance-report Timers block. The external budget the Python harness imposes with
/// subprocess.run(timeout=) sends SIGKILL, which is uncatchable and leaves no evidence at all; that
/// external kill stays as a backstop, but this deadline is meant to fire first. A handler on a
/// catchable signal would not help either -- lib/crash.cpp already binds SIGHUP/INT/QUIT/TERM, and
/// a handler cannot safely touch boost::format or an ostream.
///
/// Uses steady_clock (NOT lib/timer.cpp's high_resolution_clock, which is not guaranteed
/// monotonic), so the budget is immune to wall-clock adjustment.
class Deadline {
 public:
    /// Records the run's t0. Called once from main(), unconditionally, so elapsedSeconds() is
    /// meaningful even when no --timeout was given.
    static void start();

    /// Sets the budget to @p seconds after t0. A zero budget arms nothing, which is the default and
    /// reproduces the pre-existing behaviour exactly. Called from the --timeout option handler, so
    /// the budget covers the frontend/midend as well as the search.
    static void arm(uint64_t seconds);

    /// Whether a non-zero budget was set.
    static bool armed();

    /// Whether the budget has been exceeded. Pure query: false when not armed, and it does not
    /// latch. Poll sites want checkAndLatch() instead.
    static bool expired();

    /// expired(), latching markHit() on the way. The one-liner for poll sites.
    static bool checkAndLatch();

    /// Latches the fact that the deadline cut the run short, so the summary can report it.
    static void markHit();

    /// Whether the run was cut short. Stays true once set.
    static bool wasHit();

    /// Seconds since t0. Valid whether or not the deadline is armed.
    static uint64_t elapsedSeconds();
};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_DEADLINE_H_ */
