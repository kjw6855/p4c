
#include "backends/p4tools/modules/symbex/lib/logging.h"

#include "lib/log.h"

namespace P4::P4Tools::Symbex {

void enableTraceLogging() { Log::addDebugSpec("test_traces:4"); }

void enableStepLogging() {
    Log::addDebugSpec("small_step:4");
    Log::addDebugSpec("small_visit:4");
}

void enableCoverageLogging() { Log::addDebugSpec("coverage:4"); }

}  // namespace P4::P4Tools::Symbex
