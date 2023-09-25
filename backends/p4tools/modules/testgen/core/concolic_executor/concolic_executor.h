#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_CORE_CONCOLIC_EXECUTOR_CONCOLIC_EXECUTOR_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_CORE_CONCOLIC_EXECUTOR_CONCOLIC_EXECUTOR_H_

#include <cstdint>
#include <list>
#include <string>
#include <vector>
#include <map>

#include <boost/optional/optional.hpp>

#include "frontends/p4/parserCallGraph.h"
#include "frontends/p4/typeMap.h"
#include "frontends/common/resolveReferences/referenceMap.h"
#include "ir/ir.h"
#include "ir/visitor.h"

#include "backends/p4tools/modules/testgen/core/small_visit/small_visit.h"
#include "backends/p4tools/modules/testgen/core/small_visit/abstract_visitor.h"
#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/lib/graphs/graphs.h"
#include "backends/p4tools/modules/testgen/lib/graphs/controls.h"
#include "backends/p4tools/modules/testgen/lib/graphs/parsers.h"
#include "backends/p4tools/modules/testgen/lib/concolic.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/lib/table_collector.h"
#include "backends/p4tools/modules/testgen/lib/final_visit_state.h"
#include "backends/p4tools/modules/testgen/lib/test_spec.h"
#include "backends/p4tools/modules/testgen/p4testgen.pb.h"

using p4testgen::TestCase;

namespace P4Tools::P4Testgen {

/// Explores one path described by a list of branches.
class ConcolicExecutor {
 public:
    ~ConcolicExecutor();

    /// Constructor for this strategy, considering inheritance
    ConcolicExecutor(const ProgramInfo &programInfo, TableCollector &tableCollector, const IR::ToplevelBlock *top, P4::ReferenceMap *refMap, P4::TypeMap *typeMap);

    /// Executes the P4 program along a randomly chosen path. When the program terminates, the
    /// given callback is invoked. If the callback returns true, then the executor terminates.
    /// Otherwise, execution of the P4 program continues on a different random path.
    void run(TestCase &testCase);

    const P4::Coverage::CoverageSet &getVisitedStatements();

    const std::string getStatementBitmapStr();
    const std::string getActionBitmapStr();

    boost::optional<Packet> getOutputPacket();

    using Branch = SmallStepEvaluator::Branch;
    using Result = SmallStepEvaluator::Result;

 protected:
    /// Target-specific information about the P4 program.
    const ProgramInfo &programInfo;
    TableCollector &tableCollector;
    const IR::ToplevelBlock *top;
    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;

    /// Chooses a branch corresponding to a given branch identifier.
    ///
    /// @returns next execution state to be examined, throws an exception on invalid nextBranch.
    ExecutionState* chooseBranch(const std::vector<Branch>& branches, uint64_t nextBranch);

    bool testHandleTerminalState(const ExecutionState &terminalState);

    big_int get_total_path(Graphs::Graph *g);

    /// The current execution state.
    std::reference_wrapper<ExecutionState> executionState;

    std::reference_wrapper<ExecutionState> tableState;

    FinalVisitState* finalState = nullptr;

    /// Set of all stetements, to be retrieved from programInfo.
    const P4::Coverage::CoverageSet &allStatements;

    /// Set of all statements executed in any testcase that has been outputted.
    P4::Coverage::CoverageSet visitedStatements;

    std::set<cstring, ControlGraphs*> cgenSet;
    ParserGraphs *pgg = nullptr;

 public:
    const int statementBitmapSize;
    const int actionBitmapSize;
    unsigned char* statementBitmap;
    unsigned char* actionBitmap;
    std::list<cstring> visitedPathComponents;
    std::map<cstring, big_int> visitedPaths;
    std::map<cstring, big_int> totalPaths;
    //const int tableEntryBitmapSize;
    //unsigned char* tableEntryBitmap;


 private:
    SmallVisitEvaluator evaluator;
    SmallVisitEvaluator tableEvaluator;
};

}  // namespace P4Tools::P4Testgen

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_CORE_CONCOLIC_EXECUTOR_CONCOLIC_EXECUTOR_H_ */
