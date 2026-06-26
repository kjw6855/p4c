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
#include "midend/parserUnroll.h"

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
#include "backends/state_dependency/chain_cache.h"
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
    // Whole-pipeline mode: unroll parser header-stack loops into distinct, uniquely-named states
    // (bounded by the stack size) so the parser CFG is acyclic with concrete stack indices before the
    // analysis walks it. Opt-in; the legacy per-control path is untouched.
    if (options.wholePipeline) {
        Util::ScopedTimer unrollTimer("Parser unroll");
        P4::ParsersUnroll parsersUnroll(true, &midEnd.refMap, &midEnd.typeMap);
        program = program->apply(parsersUnroll);
        if (program == nullptr || ::P4::errorCount() > 0) return 1;
    }
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
                    options.graphsDir, P4StateDependency::SD_ALL, options.wholePipeline);
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

    // Serialize the Key + Cond chains so p4symbex can reuse them (--state-dep-cache), skipping the
    // expensive IFDS re-analysis. Requires the analysis to have run (--gen-supergraphs).
    if (options.cacheChainsFile.has_value()) {
        cstring lang = options.langVersion == CompilerOptions::FrontendVersion::P4_14
                           ? cstring("p4-14")
                           : cstring("p4-16");
        cstring srcHash =
            P4StateDependency::computeSourceHash(options.file.string(), options.arch, lang);
        P4StateDependency::serializeChainCache(sdResult, *options.cacheChainsFile, srcHash,
                                               options.arch);
        if (::P4::errorCount() == 0)
            std::cout << "Wrote chain cache to " << *options.cacheChainsFile << "\n";
    }

    // Print per-control-block and total dependency chain counts.
    if (sdResult.cfgGraphs &&
            (sdResult.h2s2vGraphs || sdResult.h2s2cGraphs || sdResult.hdChecker)) {
        const auto &cfgArr = sdResult.cfgGraphs->controlGraphsArray;
        const size_t numGraphs = cfgArr.size();

        // Case 1: H2S2V reads of registers that are NOT written by header data in the dep graph
        //         (SO vertices with no incoming "write_to" edge → their reachable leaves).
        // Case 2: DATA-write chains whose sink is a match key (VertexFlags::KEY).
        // Case 3: DATA-write chains whose sink is a header/port field (non-KEY).
        // Case 4: Same as case 2/3 but restricted to H2S2C (SO value reaches a condition leaf).
        size_t noWriteReadTotal = 0;
        size_t dataWriteToKeyTotal = 0;
        size_t dataWriteToHdrTotal = 0;
        size_t dataWriteToCondTotal = 0;

        std::cout << "\n================ State Dependency Counts ================\n";
        for (size_t i = 0; i < numGraphs; i++) {
            auto graphName = cstring(boost::get_property(*cfgArr[i], boost::graph_name));
            size_t noWriteRead = sdResult.noWriteReadChains[graphName].size();
            size_t dataWriteToKey = sdResult.dataWriteKeyChains[graphName].size();
            size_t dataWriteToHdr = sdResult.dataWriteHeaderChains[graphName].size();
            size_t dataWriteToCond = sdResult.dataWriteCondChains[graphName].size();
            size_t dataWrite = dataWriteToKey + dataWriteToHdr + dataWriteToCond;

            if (noWriteRead == 0 && dataWrite == 0) continue;

            std::cout << "  [" << graphName << "]\n"
                      << "    (1) H2S2V non-write SO reads:     " << noWriteRead << "\n"
                      << "    (2) H->SO DATA writes to key:     " << dataWriteToKey << "\n"
                      << "    (3) H->SO DATA writes to header:  " << dataWriteToHdr << "\n"
                      << "    (4) H2S2C DATA writes to cond:    " << dataWriteToCond << "\n";
            noWriteReadTotal += noWriteRead;
            dataWriteToKeyTotal += dataWriteToKey;
            dataWriteToHdrTotal += dataWriteToHdr;
            dataWriteToCondTotal += dataWriteToCond;
        }
        std::cout << "  --- Total ---\n"
                  << "    (1) non-write SO reads:     " << noWriteReadTotal << "\n"
                  << "    (2) DATA writes to key:     " << dataWriteToKeyTotal << "\n"
                  << "    (3) DATA writes to header:  " << dataWriteToHdrTotal << "\n"
                  << "    (4) DATA writes to cond:    " << dataWriteToCondTotal << "\n"
                  << "=========================================================\n\n";
    }

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
