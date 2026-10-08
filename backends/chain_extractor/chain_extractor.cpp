/**
 * @author Jiwon Kim
 */

#include "backends/chain_extractor/version.h"
#include "backends/chain_extractor/chain_extractor.h"
#include "backends/chain_extractor/options.h"
#include "backends/chain_extractor/post_midend.h"

#include "frontends/common/applyOptionsPragmas.h"
#include "frontends/common/parseInput.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "frontends/p4/frontend.h"
#include "midend/eliminateSerEnums.h"
#include "midend/parserUnroll.h"

#include "backends/ifds/graphs.h"
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
#include "backends/chain_extractor/chain_cache.h"
#include "backends/ifds/graphs.h"
#include "backends/ifds/parsers.h"
#include "backends/ifds/graph_visitor.h"

namespace P4::P4ChainExtractor {

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

}  // namespace P4::P4ChainExtractor

using namespace P4;
using P4ChainExtractorContext = P4CContextWithOptions<::P4ChainExtractor::P4ChainExtractorOptions>;

using VarEdgeVisibility = P4ChainExtractor::VarEdgeVisibility;

int main(int argc, char *const argv[]) {
    setup_gc_logging();
    setup_signals();

    AutoCompileContext autoP4ChainExtractorContext(new P4ChainExtractorContext);
    auto &options = P4ChainExtractorContext::get().options();

    options.langVersion = CompilerOptions::FrontendVersion::P4_16;
    options.compilerVersion = cstring(P4C_CHAIN_EXTRACTOR_VERSION_STRING);

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

    // --post-midend: reproduce the IR level p4symbex analyzes before anything else touches the
    // program. Everything below -- the evaluator MidEnd, the analysis, the counts, the graphs --
    // then runs unchanged on the lowered IR.
    if (options.postMidEnd) {
        Util::ScopedTimer postMidEndTimer("P4 post-midend");
        try {
            program = P4ChainExtractor::applyPostMidEnd(program, options);
        } catch (const std::exception &bug) {
            std::cerr << bug.what() << std::endl;
            return 1;
        }
        if (program == nullptr || ::P4::errorCount() > 0) return 1;
    }

    P4ChainExtractor::MidEnd midEnd(options);
    midEnd.addDebugHook(hook);
    // Whole-pipeline mode: unroll parser header-stack loops into distinct, uniquely-named states
    // (bounded by the stack size) so the parser CFG is acyclic with concrete stack indices before the
    // analysis walks it. Opt-in; the legacy per-control path is untouched.
    // Skipped under --post-midend: that pass list already runs EliminateSerEnums and ParsersUnroll
    // (backends/p4tools/common/compiler/midend.cpp:100,151), so this would be redundant work.
    if ((options.wholePipeline || options.parserDeps) && !options.postMidEnd) {
        Util::ScopedTimer unrollTimer("Parser unroll");
        // ParsersUnroll's symbolic interpreter can throw (e.g. CompilerBug on Type_Newtype) or set
        // errors on some programs. Treat unrolling as best-effort: on any failure keep the original
        // program — the parser walk's visited-set still cuts loops (over-approximate, no crash).
        try {
            unsigned errBefore = ::P4::errorCount();
            // Lower serializable enums to their underlying bit<> type first. ParsersUnroll's symbolic
            // interpreter (midend/interpreter.cpp) has no Type_SerEnum case and BUGs out on it; the normal
            // midend runs EliminateSerEnums well before any such interpreter, so mirror that ordering here.
            // EliminateSerEnums ends with ClearTypeMap, so re-typecheck to repopulate typeMap for unroll.
            PassManager serEnumPrep;
            serEnumPrep.addPasses({
                new P4::TypeChecking(&midEnd.refMap, &midEnd.typeMap, true),
                new P4::EliminateSerEnums(&midEnd.typeMap),
                new P4::TypeChecking(&midEnd.refMap, &midEnd.typeMap, true),
            });
            program = program->apply(serEnumPrep);
            P4::ParsersUnroll parsersUnroll(true, &midEnd.refMap, &midEnd.typeMap);
            const auto *unrolled = program->apply(parsersUnroll);
            if (unrolled != nullptr && ::P4::errorCount() == errBefore)
                program = unrolled;
            else
                ::P4::warning("whole-pipeline: parser unroll failed; using visited-set loop cut");
        } catch (const std::exception &e) {
            ::P4::warning("whole-pipeline: parser unroll threw (%1%); using visited-set loop cut",
                          e.what());
        }
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

    P4ChainExtractor::ParserGraphs *pgg = nullptr;
    P4ChainExtractor::StateDependencyResult sdResult;

    {
        LOG2("Generating graphs under " << options.graphsDir);
        if (options.genSupergraphs != P4ChainExtractor::GenSGMode::NONE) {
            // Whole-pipeline modeling of real (esp. tna) programs can still hit assertions in the
            // block-as-procedure / var-registration machinery. Isolate it: a crash in WP mode yields
            // an empty result (no chains for this program) rather than aborting. Per-control stays
            // strict (no catch) so genuine regressions still surface.
            try {
                sdResult = P4ChainExtractor::runStateDependencyAnalysis(
                        program, &midEnd.refMap, &midEnd.typeMap, top, options.arch,
                        options.graphsDir, P4ChainExtractor::SD_FOUR, options.wholePipeline,
                        options.parserDeps, options.supergraphOnly);
            } catch (const std::exception &bug) {
                if (!options.wholePipeline) throw;
                ::P4::warning("whole-pipeline analysis failed (%1%); emitting no chains for this "
                              "program", bug.what());
                sdResult = P4ChainExtractor::StateDependencyResult{};
            }
            if (sdResult.cfgGraphs)
                sdResult.cfgGraphs->varVis = options.varVis;
        } else {
            // CFG only (no supergraphs / IFDS analysis).
            sdResult.cfgGraphs = new P4ChainExtractor::ControlGraphs(
                    &midEnd.refMap, &midEnd.typeMap, options.graphsDir, options.arch);
            sdResult.cfgGraphs->varVis = options.varVis;
            sdResult.cfgGraphs->genSupergraphs = P4ChainExtractor::GenSGMode::NONE;
            top->getMain()->apply(*sdResult.cfgGraphs);
        }
    }

    // Serialize the Key + Cond chains so p4symbex can reuse them (--state-dep-cache), skipping the
    // expensive IFDS re-analysis. Requires the analysis to have run (--gen-supergraphs).
    if (options.cacheChainsFile.has_value() && options.supergraphOnly) {
        // --supergraph-only produces no chains; writing the cache here would silently truncate an
        // existing one to zero entries. Refuse instead of clobbering it.
        ::P4::error("--cache-chains cannot be combined with --supergraph-only (no chains are "
                    "computed in measurement-only mode); the cache was left untouched");
        return 1;
    }
    if (options.cacheChainsFile.has_value()) {
        cstring lang = options.langVersion == CompilerOptions::FrontendVersion::P4_14
                           ? cstring("p4-14")
                           : cstring("p4-16");
        cstring srcHash =
            P4ChainExtractor::computeSourceHash(options.file.string(), options.arch, lang);
        P4ChainExtractor::serializeChainCache(sdResult, *options.cacheChainsFile, srcHash,
                                               options.arch);
        if (::P4::errorCount() == 0)
            std::cout << "Wrote chain cache to " << *options.cacheChainsFile << "\n";
    }

    // Print per-control-block and total dependency chain counts.
    //
    // Four relations, each named by its full source -> stateful object -> sink path. The two
    // value-sink families that used to be reported here -- H2S2V (H -> SO -> header/port field)
    // and A2S2V (action param -> SO -> header/port field) -- and the "non-write SO reads" count
    // are no longer computed (see SD_FOUR) or printed: nothing downstream consumes them
    // (analysis/funnel.py's CHAIN_CATS is exactly these four).
    if (sdResult.cfgGraphs != nullptr) {
        const auto &cfgArr = sdResult.cfgGraphs->controlGraphsArray;
        const size_t numGraphs = cfgArr.size();

        // Column-aligned "<CODE>  <relation>:" labels. read_log.py keys on these exact strings.
        static const char *const H2S2K_LABEL = "H2S2K  header -> SO -> match key:       ";
        static const char *const H2S2C_LABEL = "H2S2C  header -> SO -> condition:       ";
        static const char *const A2S2K_LABEL = "A2S2K  action param -> SO -> match key: ";
        static const char *const A2S2C_LABEL = "A2S2C  action param -> SO -> condition: ";

        size_t h2s2kTotal = 0;
        size_t h2s2cTotal = 0;
        size_t a2s2kTotal = 0;
        size_t a2s2cTotal = 0;

        std::cout << "\n================ State Dependency Counts ================\n";
        for (size_t i = 0; i < numGraphs; i++) {
            auto graphName = cstring(boost::get_property(*cfgArr[i], boost::graph_name));
            size_t h2s2k = sdResult.dataWriteKeyChains[graphName].size();
            size_t h2s2c = sdResult.dataWriteCondChains[graphName].size();
            size_t a2s2k = sdResult.dataWriteA2SKeyChains[graphName].size();
            size_t a2s2c = sdResult.dataWriteA2SCondChains[graphName].size();

            h2s2kTotal += h2s2k;
            h2s2cTotal += h2s2c;
            a2s2kTotal += a2s2k;
            a2s2cTotal += a2s2c;

            if (h2s2k == 0 && h2s2c == 0 && a2s2k == 0 && a2s2c == 0) continue;

            std::cout << "  [" << graphName << "]\n"
                      << "    " << H2S2K_LABEL << h2s2k << "\n"
                      << "    " << H2S2C_LABEL << h2s2c << "\n"
                      << "    " << A2S2K_LABEL << a2s2k << "\n"
                      << "    " << A2S2C_LABEL << a2s2c << "\n";
        }
        std::cout << "  --- Total ---\n"
                  << "    " << H2S2K_LABEL << h2s2kTotal << "\n"
                  << "    " << H2S2C_LABEL << h2s2cTotal << "\n"
                  << "    " << A2S2K_LABEL << a2s2kTotal << "\n"
                  << "    " << A2S2C_LABEL << a2s2cTotal << "\n";
        // Distinct stateful-object (register) name sets. These are NOT chain types: H2S/A2S are the
        // first-hop sets and ALL / TYPE:* are the D_all / D_crao / D_rao denominators, from which
        // read_log.py derives every reg_* and frac_* column. read_log parses the space-separated
        // names after each label into a set.
        auto printSOSet = [](const char *label, const std::set<cstring> &s) {
            std::cout << "    SOs[" << label << "]:";
            for (const auto &n : s) std::cout << " " << n;
            std::cout << "\n";
        };
        printSOSet("A2S", sdResult.regA2S);
        printSOSet("H2S", sdResult.regH2S);
        printSOSet("A2S2K", sdResult.regA2S2K);
        printSOSet("A2S2C", sdResult.regA2S2C);
        printSOSet("H2S2K", sdResult.regH2S2K);
        printSOSet("H2S2C", sdResult.regH2S2C);
        printSOSet("ALL", sdResult.allStatefulObjects);
        // Per-type declared-SO sets -> the D_all/D_crao/D_rao denominators (read_log unions them).
        // Always emit the four so the denominator columns are present even at 0.
        std::map<cstring, std::set<cstring>> byType;
        for (const auto &[soName, soType] : sdResult.soTypeByName) byType[soType].insert(soName);
        for (const char *type : {"Register", "Counter", "Meter", "AddOnMiss"}) {
            std::cout << "    SOs[TYPE:" << type << "]:";
            for (const auto &n : byType[cstring(type)]) std::cout << " " << n;
            std::cout << "\n";
        }
        std::cout << "=========================================================\n\n";
    }

    {
        // Runs in --supergraph-only too: ParserGraphs only builds the parser graphs in memory
        // (DOT writing happens in the drawing pass below), so it belongs to the graph-generation
        // latency being measured.
        Util::ScopedTimer parserTimer("Parser graphs");
        LOG2("Generating parser graphs");
        pgg = new P4ChainExtractor::ParserGraphs(&midEnd.refMap, options.graphsDir);
        program->apply(*pgg);
    }

    // Drawing is disk output, not graph generation — skipped in the measurement-only mode.
    if (!options.supergraphOnly) {
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
            P4ChainExtractor::GraphVisitor gvs(options.graphsDir, options.graphs,
                    options.fullGraph, options.jsonOut, options.file,
                    options.varVis, options.varEdgeVis);
            gvs.process(sdResult.cfgGraphs->controlGraphsArray, pgg->parserGraphsArray);
        }
    }

    P4ChainExtractor::printPerformanceReport();

    delete pgg;
    delete sdResult.s2vChecker;
    delete sdResult.sdChecker;
    delete sdResult.hdChecker;
    delete sdResult.cfgGraphs;
    delete sdResult.h2s2vGraphs;
    delete sdResult.a2s2vGraphs;
    return ::P4::errorCount() > 0;
}
