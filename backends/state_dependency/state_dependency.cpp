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

#include "graphs.h"
#include "controls.h"
#include "parsers.h"
#include "graph_visitor.h"
#include "supergraphs.h"
#include "act_param_to_stateful.h"
#include "stateful_to_key.h"

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
        program = new IR::P4Program(jsonFileLoader);
        fb.close();
    } else {
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
    try {
        top = midEnd.process(program);
        if (!options.dumpJsonFile.empty())
            JSONGenerator(*openFile(options.dumpJsonFile, true)).emit(program);
    } catch (const std::exception &bug) {
        std::cerr << bug.what() << std::endl;
        return 1;
    }
    if (::P4::errorCount() > 0) return 1;

    LOG2("Generating graphs under " << options.graphsDir);
    LOG2("Generating control graphs");
    P4StateDependency::ControlGraphs cgen(&midEnd.refMap, &midEnd.typeMap,
            options.graphsDir);
    // TODO: set options in contructor
    cgen.varVis = options.varVis;
    cgen.genSupergraphs = options.genSupergraphs;
    top->getMain()->apply(cgen);

    if (options.genSupergraphs != P4StateDependency::GenSGMode::NONE) {
        P4StateDependency::SuperGraphs sg(&midEnd.refMap, &midEnd.typeMap,
                &cgen.controlGraphsArray,
                &cgen.graphVars,
                &cgen.graphLocalVars,
                &cgen.procOfs,
                &cgen.callMaps,
                &cgen.procCallerMaps,
                &cgen.retArgEdges,
                &cgen.actionMaps);

        // generate supergraphs
        sg.gen_supergraphs();

        // State dependency checker
        P4StateDependency::ActParamToStateful sdChecker(&midEnd.refMap, &midEnd.typeMap,
                &cgen.controlGraphsArray,
                &sg.graphProps,
                options.genSupergraphs);
        program->apply(sdChecker);

        for (size_t i = 0; i < cgen.controlGraphsArray.size(); i++) {
            auto *g = cgen.controlGraphsArray[i];
            auto graphName = boost::get_property(*g, boost::graph_name);
            for (const auto &ve : sdChecker.getFoundDepEdges(graphName)) {
                LOG2(P4StateDependency::Graphs::dump_var_edge(g, ve));
            }
        }

        // Packet dependency checker
        P4StateDependency::StatefulToKey pdChecker(&midEnd.refMap, &midEnd.typeMap,
                &cgen.controlGraphsArray,
                &sg.graphProps,
                options.genSupergraphs,
                sdChecker.getAllFoundDepEdges());
        program->apply(pdChecker);

        if (options.varEdgeVis == VarEdgeVisibility::ACTION_PARAM ||
                options.varEdgeVis == VarEdgeVisibility::ALL) {
            sdChecker.set_edge_func();
        }
        if (options.varEdgeVis == VarEdgeVisibility::STATEFUL_OBJECT ||
                options.varEdgeVis == VarEdgeVisibility::ALL) {
            pdChecker.set_edge_func();
        }
    }

    LOG2("Generating parser graphs");
    P4StateDependency::ParserGraphs pgg(&midEnd.refMap, options.graphsDir);
    program->apply(pgg);

    P4StateDependency::GraphVisitor gvs(options.graphsDir, options.graphs,
            options.fullGraph, options.jsonOut, options.file,
            options.varVis, options.varEdgeVis);

    gvs.process(cgen.controlGraphsArray, pgg.parserGraphsArray);

    return ::P4::errorCount() > 0;
}
