#include <exception>
#include <iostream>
#include <vector>

#include "lib/crash.h"
#include "lib/timer.h"

#include "backends/p4tools/modules/symbex/lib/deadline.h"
#include "backends/p4tools/modules/symbex/lib/logging.h"
#include "backends/p4tools/modules/symbex/symbex.h"
#include "backends/p4tools/modules/symbex/toolname.h"

using namespace P4;

std::string updateErrorMsg(std::string errorMsg) {
    for (const std::string_view toReplace : {"Compiler", "compiler"}) {
        if (const auto pos = errorMsg.find(toReplace); pos != std::string::npos) {
            errorMsg.replace(pos, toReplace.size(), "Symbex");
            break;
        }
    }

    return errorMsg;
}

int main(int argc, char **argv) {
    setup_signals();
    // t0 for --timeout and for the elapsed= field of the [Symbex Summary] line. Started before
    // anything else so the budget covers option parsing and the frontend/midend, not just the
    // search -- a run can die in the frontend, and that is precisely a case we cannot currently
    // tell apart from dying in the search.
    P4::P4Tools::Symbex::Deadline::start();

    std::vector<const char *> args;
    args.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        args.push_back(argv[i]);
    }

    int result = EXIT_SUCCESS;
    try {
        Util::ScopedTimer timer("symbex_main");
        result = P4::P4Tools::Symbex::Symbex().main(P4::P4Tools::Symbex::TOOL_NAME, args);
    } catch (const Util::CompilerBug &e) {
        std::cerr << "Internal error: " << updateErrorMsg(e.what()) << "\n";
        std::cerr << "Please submit a bug report with your code." << "\n";
        result = EXIT_FAILURE;
    } catch (const Util::CompilerUnimplemented &e) {
        std::cerr << updateErrorMsg(e.what()) << "\n";
        result = EXIT_FAILURE;
    } catch (const Util::CompilationError &e) {
        std::cerr << updateErrorMsg(e.what()) << "\n";
        result = EXIT_FAILURE;
    } catch (const std::exception &e) {
        std::cerr << "Internal error: " << updateErrorMsg(e.what()) << "\n";
        std::cerr << "Please submit a bug report with your code." << "\n";
        result = EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Internal error. Please submit a bug report with your code." << "\n";
        result = EXIT_FAILURE;
    }
    // On stderr, so a run cut short is flagged even without --print-performance-report. The exit
    // code stays EXIT_SUCCESS: a self-imposed deadline is a PARTIAL success, and the tests
    // already written to disk are valid. The [Symbex Summary] status= field carries the truth.
    if (P4::P4Tools::Symbex::Deadline::wasHit()) {
        std::cerr << "[Symbex] deadline reached after "
                  << P4::P4Tools::Symbex::Deadline::elapsedSeconds()
                  << "s; results are PARTIAL" << std::endl;
    }
    P4Tools::printPerformanceReport();
    return result;
}
