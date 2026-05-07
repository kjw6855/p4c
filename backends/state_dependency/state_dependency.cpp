/**
 * @author Jiwon Kim
 */

#include "backends/state_dependency/version.h"
#include "backends/state_dependency/state_dependency.h"
#include "backends/state_dependency/options.h"

#include "frontends/common/applyOptionsPragmas.h"
#include "frontends/common/parseInput.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "frontends/p4/frontend.h"

#include "backends/state_dependency/graphs.h"
#include "ir/ir.h"
#include "ir/json_loader.h"
#include "ir/pass_utils.h"
#include "lib/crash.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/gc.h"
#include "lib/log.h"
#include "lib/nullstream.h"
#include "lib/timer.h"

#include "analysis.h"
#include "graphs.h"
#include "parsers.h"
#include "graph_visitor.h"

namespace P4::P4StateDependency {

class MidEnd : public PassManager {
 public:
    P4::ReferenceMap refMap;
    P4::TypeMap typeMap;
    IR::ToplevelBlock *toplevel = nullptr;

    explicit MidEnd(CompilerOptions &options);
    IR::ToplevelBlock *process(const IR::P4Program *&program) {
        program = program->apply(*this);
        return toplevel;
    }
};

MidEnd::MidEnd(CompilerOptions &options) {
    bool isv1 = options.langVersion == CompilerOptions::FrontendVersion::P4_14;
    refMap.setIsV1(isv1);
    auto evaluator = new P4::EvaluatorPass(&refMap, &typeMap);
    setName("MidEnd");

    addPasses({
        new P4::TypeChecking(&refMap, &typeMap, true),  // update types before ComputeDefUse
        evaluator,
        [this, evaluator]() { toplevel = evaluator->getToplevelBlock(); },
    });
}

}  // namespace P4::P4StateDependency

using namespace P4;
using P4StateDependencyContext = P4CContextWithOptions<::P4StateDependency::P4StateDependencyOptions>;

using VarEdgeVisibility = P4StateDependency::VarEdgeVisibility;

int main(int argc, char *const argv[]) {
    setup_gc_logging();
    setup_signals();

    AutoCompileContext autoP4StateDependencyContext(new P4StateDependencyContext);
    auto &options = P4StateDependencyContext::get().options();

    options.langVersion = CompilerOptions::FrontendVersion::P4_16;
    options.compilerVersion = cstring(P4C_STATE_DEPENDENCY_VERSION_STRING);

    if (options.process(argc, argv) != nullptr) {
        if (options.loadIRFromJson == false) options.setInputFile();
    }
    if (::P4::errorCount() > 0) return 1;

    auto hook = options.getDebugHook();

    const IR::P4Program *program = nullptr;

    if (options.loadIRFromJson) {
        std::filebuf fb;
        if (fb.open(options.file, std::ios::in) == nullptr) {
            ::P4::error(ErrorType::ERR_IO, "%s: No such file or directory.", options.file);
            return 1;
        }

        std::istream inJson(&fb);
        JSONLoader jsonFileLoader(inJson);
        if (!jsonFileLoader) {
            ::P4::error(ErrorType::ERR_IO, "Not valid input file");
            return 1;
        }
        Util::ScopedTimer jsonTimer("json loading");
        program = new IR::P4Program(jsonFileLoader);
        fb.close();
    } else {
        Util::ScopedTimer frontendTimer("P4 compile frontend");
        program = P4::parseP4File(options);
        if (program == nullptr || ::P4::errorCount() > 0) return 1;

        try {
            P4::P4COptionPragmaParser optionsPragmaParser(false);
            program->apply(P4::ApplyOptionsPragmas(optionsPragmaParser));

            P4::FrontEnd fe;
            fe.addDebugHook(hook);
            program = fe.run(options, program);
        } catch (const std::exception &bug) {
            std::cerr << bug.what() << std::endl;
            return 1;
        }
        if (program == nullptr || ::P4::errorCount() > 0) return 1;
    }

    P4StateDependency::MidEnd midEnd(options);
    midEnd.addDebugHook(hook);
    const IR::ToplevelBlock *top = nullptr;
    {
        Util::ScopedTimer midendTimer("P4 compile midend");
        try {
            top = midEnd.process(program);
            if (!options.dumpJsonFile.empty())
                JSONGenerator(*openFile(options.dumpJsonFile, true)).emit(program);
        } catch (const std::exception &bug) {
            std::cerr << bug.what() << std::endl;
            return 1;
        }
    }
    if (::P4::errorCount() > 0) return 1;

    BUG_CHECK(options.arch, "Architecture must be specified with --arch option");

    P4StateDependency::ParserGraphs *pgg = nullptr;
    P4StateDependency::StateDependencyResult sdResult;

    {
        LOG2("Generating graphs under " << options.graphsDir);
        if (options.genSupergraphs != P4StateDependency::GenSGMode::NONE) {
            sdResult = P4StateDependency::runStateDependencyAnalysis(
                    program, &midEnd.refMap, &midEnd.typeMap, top, options.arch,
                    options.graphsDir);
            if (sdResult.cfgGraphs)
                sdResult.cfgGraphs->varVis = options.varVis;
        } else {
            // CFG only (no supergraphs / IFDS analysis).
            sdResult.cfgGraphs = new P4StateDependency::ControlGraphs(
                    &midEnd.refMap, &midEnd.typeMap, options.graphsDir, options.arch);
            sdResult.cfgGraphs->varVis = options.varVis;
            sdResult.cfgGraphs->genSupergraphs = P4StateDependency::GenSGMode::NONE;
            top->getMain()->apply(*sdResult.cfgGraphs);
        }
    }

    /*
     * TODO: Show the full dependency:
     * e.g., Data Plane Dependency: Set of header types -> stateful object -> header/key
     * e.g., Control Plane Dependency: Action parameter -> stateful object -> header/key
     */
    {
        Util::ScopedTimer parserTimer("Parser graphs");
        LOG2("Generating parser graphs");
        pgg = new P4StateDependency::ParserGraphs(&midEnd.refMap, options.graphsDir);
        program->apply(*pgg);
    }

    {
        Util::ScopedTimer drawTimer("Drawing graphs");
        if (sdResult.sdChecker &&
                (options.varEdgeVis == VarEdgeVisibility::ACTION_PARAM ||
                 options.varEdgeVis == VarEdgeVisibility::ALL)) {
            sdResult.sdChecker->set_edge_func();
        }
        if (sdResult.s2vChecker &&
                (options.varEdgeVis == VarEdgeVisibility::STATEFUL_OBJECT ||
                 options.varEdgeVis == VarEdgeVisibility::ALL)) {
            sdResult.s2vChecker->set_edge_func();
        }
        if (sdResult.hdChecker &&
                (options.varEdgeVis == VarEdgeVisibility::HDR_TO_STATEFUL ||
                 options.varEdgeVis == VarEdgeVisibility::ALL)) {
            sdResult.hdChecker->set_edge_func();
        }
        if (sdResult.cfgGraphs) {
            P4StateDependency::GraphVisitor gvs(options.graphsDir, options.graphs,
                    options.fullGraph, options.jsonOut, options.file,
                    options.varVis, options.varEdgeVis);
            gvs.process(sdResult.cfgGraphs->controlGraphsArray, pgg->parserGraphsArray);
        }
    }

    P4StateDependency::printPerformanceReport();

    delete pgg;
    delete sdResult.s2vChecker;
    delete sdResult.sdChecker;
    delete sdResult.hdChecker;
    delete sdResult.cfgGraphs;
    delete sdResult.h2s2vGraphs;
    delete sdResult.a2s2vGraphs;
    return ::P4::errorCount() > 0;
}
