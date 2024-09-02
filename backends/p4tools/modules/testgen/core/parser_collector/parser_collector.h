#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_CORE_PARSER_COLLECTOR_PARSER_COLLECTOR_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_CORE_PARSER_COLLECTOR_PARSER_COLLECTOR_H_

#include <vector>

#include "backends/p4tools/modules/testgen/core/small_parse_get/small_parse_get.h"
#include "backends/p4tools/modules/testgen/core/small_parse_get/abstract_parse_getter.h"
#include "backends/p4tools/modules/testgen/core/small_step/small_step.h"
#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/lib/final_visit_state.h"
#include "backends/p4tools/modules/testgen/p4testgen.pb.h"

using p4testgen::P4ParserGraph;
using p4testgen::P4ParserGraphNode;
using p4testgen::P4ParserGraphEdge;

namespace P4Tools::P4Testgen {

class ParserCollector {
 public:
    //virtual ~ParserCollector() = default;

    ParserCollector(const ProgramInfo &programInfo, const IR::ToplevelBlock *top,
            P4::ReferenceMap *refMap, P4::TypeMap *typeMap, P4ParserGraph *parserGraph);

    void run();

    using Branch = SmallStepEvaluator::Branch;
    using Result = SmallStepEvaluator::Result;

 protected:
    /// Target-specific information about the P4 program.
    const ProgramInfo &programInfo;
    const IR::ToplevelBlock *top;
    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;
    P4ParserGraph *parserGraph;

    /// The current execution state.
    ExecutionStateReference executionState;

 private:
    std::vector<Branch> unexploredBranches;
    [[nodiscard]] std::optional<ExecutionStateReference> pickSuccessor(Result successors);
    SmallParseGetEvaluator evaluator;
};

}  // namespace P4Tools::P4Testgen

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_CORE_PARSER_COLLECTOR_PARSER_COLLECTOR_H_ */
