#include "backends/p4tools/modules/testgen/core/concolic_executor/concolic_executor.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <boost/variant/apply_visitor.hpp>
#include <boost/variant/static_visitor.hpp>
#include <boost/graph/topological_sort.hpp>
#include <boost/graph/adjacency_list.hpp>

#include "backends/p4tools/common/lib/taint.h"
#include "ir/ir.h"
#include "ir/irutils.h"
#include "ir/visitor.h"
#include "ir/node.h"
#include "lib/gc.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/null.h"
#include "lib/timer.h"
#include "midend/coverage.h"

#include "backends/p4tools/common/lib/util.h"
#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/lib/test_spec.h"
#include "backends/p4tools/modules/testgen/lib/visit_concolic.h"
#include "backends/p4tools/modules/testgen/lib/graphs/graphs.h"
#include "backends/p4tools/modules/testgen/lib/graphs/controls.h"
#include "backends/p4tools/modules/testgen/lib/graphs/parsers.h"

namespace P4Tools::P4Testgen {
inline void hash_combine(std::size_t& seed) { }

template <typename T, typename... Rest>
inline void hash_combine(std::size_t& seed, const T& v, Rest... rest) {
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed<<6) + (seed>>2);
    hash_combine(seed, rest...);
}

big_int ConcolicExecutor::get_total_path(Graphs::Graph *g) {
    Graphs::OutEdgeIterator eit, eend;
    typedef std::vector<Graphs::vertex_t> container;
    container c;

    auto subg = *g;
    boost::topological_sort(subg, std::back_inserter(c));
    LOG_FEATURE("small_visit", 4, "A topological ordering of "
            << boost::get_property(subg, boost::graph_name));
    big_int totalPath = 0;
    for (container::reverse_iterator ii=c.rbegin(); ii!=c.rend(); ++ii) {
        auto v = subg[*ii];
        if (totalPath < v.numPaths)
            totalPath = v.numPaths;

        std::stringstream sstream;
        if (v.node != nullptr)
            sstream << v.node;
        else
            sstream << v.name;
        sstream << " (" << v.numPaths << " paths)";
        LOG_FEATURE("small_visit", 4, cstring(sstream));
        std::tie(eit, eend) = boost::out_edges(*ii, subg);
        for (; eit != eend; eit++) {
            auto e = *eit;
            auto u = boost::target(e, subg);
            LOG_FEATURE("small_visit", 4, "Edge weight on Graph " << static_cast<void*>(g)
                    << " from " << *ii
                    << " to " << u
                    << ": " << boost::get(boost::edge_weight, g->root(), e));
        }
    }
    return totalPath;
}

std::size_t ConcolicExecutor::get_rule_key(TestCase &testCase) {
    std::size_t h = 0;
    for (const auto &entity : testCase.entities()) {
        if (!entity.has_table_entry())
            continue;

        const auto &entry = entity.table_entry();
        if (!entry.is_valid_entry())
            continue;

        hash_combine<std::string>(h, entity.SerializeAsString());
    }

    return h;
}

void ConcolicExecutor::run(TestCase& testCase) {
    executionState = ExecutionState::create(programInfo.program);

    Continuation::Body body(tableCollector.getP4Tables());
    const auto actionNodes = tableCollector.getActionNodes();
    body.push(programInfo.program);

    tableState = ExecutionState::create(programInfo.program, body);

    tableEvaluator.violatedGuardConditions = 0;
    evaluator.violatedGuardConditions = 0;

    // 1. Verify validity first
    while (!tableState.get().isTerminal()) {
        LOG_FEATURE("small_visit", 4, " [T] stack/body size: " << tableState.get().getStackSize() << "/" << tableState.get().getBodySize());

        Result successors = tableEvaluator.step(tableState, testCase);

        if (successors->size() == 1) {
            // Non-branching states are not recorded by selected branches.
            tableState = (*successors)[0].nextState;
            continue;
        } else if (successors->size() == 0) {
            continue;
        }

        // If there are multiple, pop one branch decision from the input list and pick
        // successor matching the given branch decision.
        auto* next = chooseBranch(*successors, 0);
        if (next == nullptr) {
            break;
        }
    }

    auto ruleKey = get_rule_key(testCase);
    ControlGraphs *cgen = nullptr;
    if (!cgenCache->exists(ruleKey)) {
        // 2. Calculate callgraphs
        LOG_FEATURE("small_visit", 3, "Generating control graphs (no rule key:"
                << std::hex << ruleKey << ")");
        cgen = new ControlGraphs(refMap, typeMap, testCase);
        top->getMain()->apply(*cgen);
        cgen->calc_ball_larus_on_graphs();
        cgenCache->put(ruleKey, cgen);
    } else {
        LOG_FEATURE("small_visit", 3, "Reuse control graphs (rule key:"
                << std::hex << ruleKey << ")");
        cgen = cgenCache->get(ruleKey);
    }
    executionState.get().setControlGraphs(cgen);

    if (pgg == nullptr) {
        LOG_FEATURE("small_visit", 4, "Generating parser graphs");
        pgg = new ParserGraphs(refMap);
        programInfo.program->apply(*pgg);
        pgg->calc_ball_larus_on_graphs();
    }
    executionState.get().setParserGraphs(pgg);

    totalPaths.clear();
    for (auto g : cgen->controlGraphsArray) {
        big_int totalPath = get_total_path(g);
        auto graphName = boost::get_property(*g, boost::graph_name);
        totalPaths.insert(std::pair<cstring, big_int>(graphName, totalPath));
    }

    for (auto g : pgg->parserGraphsArray) {
        big_int totalPath = get_total_path(g);
        auto graphName = boost::get_property(*g, boost::graph_name);
        totalPaths.insert(std::pair<cstring, big_int>(graphName, totalPath));
    }


    // 3. Measure coverage and calculate expected output
    while (!executionState.get().isTerminal()) {

        LOG_FEATURE("small_visit", 4, " stack/body size: " << executionState.get().getStackSize() << "/" << executionState.get().getBodySize());

        Result successors = evaluator.step(executionState, testCase);

        if (successors->size() == 1) {
            // Non-branching states are not recorded by selected branches.
            executionState = (*successors)[0].nextState;
            continue;
        }
        // If there are multiple, pop one branch decision from the input list and pick
        // successor matching the given branch decision.
        auto* next = chooseBranch(*successors, 0);
        if (next == nullptr) {
            break;
        }
        executionState = *next;
    }

    if (executionState.get().isTerminal()) {
        // We've reached the end of the program. Call back and (if desired) end execution.
        executionState.get().storeGraphPath();
        if (executionState.get().getUnsupported()) {
            testCase.set_unsupported(1);
        }
        testHandleTerminalState(executionState);
        return;
    }
}

/*
uint64_t getNumeric(const std::string& str) {
    char* leftString = nullptr;
    uint64_t number = strtoul(str.c_str(), &leftString, 10);
    BUG_CHECK(!(*leftString), "Can't translate selected branch %1% into int", str);
    return number;
}
*/

void ConcolicExecutor::setGenRuleMode(bool genRuleMode) {
    evaluator.genRuleMode = genRuleMode;
}

ConcolicExecutor::ConcolicExecutor(const ProgramInfo& programInfo, TableCollector &tableCollector, const IR::ToplevelBlock *top, P4::ReferenceMap *refMap, P4::TypeMap *typeMap)
    : programInfo(programInfo),
      tableCollector(tableCollector),
      top(top),
      refMap(refMap),
      typeMap(typeMap),
      executionState(ExecutionState::create(programInfo.program)),
      tableState(ExecutionState::create(programInfo.program)),
      allStatements(programInfo.getCoverableNodes()),
      statementBitmapSize(allStatements.size()),
      cgenCache(new lru_cache(16)),
      actionBitmapSize(tableCollector.getActionNodes().size()),
      evaluator(programInfo),
      tableEvaluator(programInfo) {

    tableEvaluator.checkTable = true;
    int allocLen = (statementBitmapSize / 8) + 1;
    statementBitmap = (unsigned char *)malloc(allocLen);
    memset(statementBitmap, 0, allocLen);

    allocLen = (actionBitmapSize / 8) + 1;
    actionBitmap = (unsigned char *)malloc(allocLen);
    memset(actionBitmap, 0, allocLen);
    //reachabilityEngine = new ReachabilityEngine(programInfo.dcg, "", true);
}

ConcolicExecutor::~ConcolicExecutor() {
    free(statementBitmap);
}

ExecutionState* ConcolicExecutor::chooseBranch(const std::vector<Branch>& branches,
                                               uint64_t nextBranch) {
    ExecutionState* next = nullptr;
    for (const auto& branch : branches) {
        const Constraint* constraint = branch.constraint;
        LOG_FEATURE("small_visit", 4, "Branch Constraint: " << constraint);

        if (const auto *boolVal = constraint->to<IR::BoolLiteral>()) {
            if (boolVal->value) {
                next = &branch.nextState.get();
                break;
            }
        } else if (Utils::isDefaultByConstraint(constraint)) {
            // Select the branch temporarily
            next = &branch.nextState.get();
        }
    }

    if (!next) {
        // If not found, the input selected branch list is invalid.
        ::error("The selected branches string doesn't match any branch.");
    }

    return next;
}

bool ConcolicExecutor::testHandleTerminalState(const ExecutionState &terminalState) {
    int i = 0;
    auto& visitedStmtSet = terminalState.getVisited();
    for (auto* stmt : allStatements) {
        if (visitedStmtSet.count(stmt)) {
            int idx = i / 8;
            int shl = 7 - (i % 8);
            statementBitmap[idx] |= 1 << shl;
        }

        i++;
    }

    auto &visitedActionSet = terminalState.getVisitedActions();
    i = 0;
    for (auto* action : tableCollector.getActionNodes()) {
        if (visitedActionSet.count(action)) {
            int idx = i / 8;
            int shl = 7 - (i % 8);
            actionBitmap[idx] |= 1 << shl;
        }
        i++;
    }

    visitedPathComponents.clear();
    visitedPaths.clear();
    auto visitedPathInState = terminalState.getVisitedPaths();
    for (auto blockName : terminalState.getVisitedPathComponents()) {
        visitedPathComponents.push_back(blockName);
        visitedPaths.insert(std::pair<cstring, big_int>(blockName, visitedPathInState[blockName]));
        std::cout << blockName << ": "
            << visitedPaths[blockName] << "/"
            << totalPaths[blockName] << "\n";
    }

    visitedParserStates.clear();
    for (auto stateName : terminalState.getVisitedParserStates()) {
        visitedParserStates.push_back(stateName);
    }

    finalState = new FinalVisitState(terminalState);

    return true;
}

const std::string ConcolicExecutor::getStatementBitmapStr() {
    int allocLen = (statementBitmapSize / 8) + 1;
    return std::string(reinterpret_cast<char*>(statementBitmap), allocLen);
}

const std::string ConcolicExecutor::getActionBitmapStr() {
    int allocLen = (actionBitmapSize / 8) + 1;
    return std::string(reinterpret_cast<char*>(actionBitmap), allocLen);
}

const P4::Coverage::CoverageSet& ConcolicExecutor::getVisitedStatements() {
    return visitedStatements;
}

boost::optional<Packet> ConcolicExecutor::getOutputPacket() {
    if (executionState.get().getProperty<bool>("drop"))
        return boost::none;

    BUG_CHECK(finalState, "Un-initialized");
    const auto &model = finalState->getFinalModel();

    const auto* outPortExpr = executionState.get().get(programInfo.getTargetOutputPortVar());
    int outPortInt = 0;
    if (dynamic_cast<const IR::Literal*>(outPortExpr) != nullptr)
        outPortInt = IR::getIntFromLiteral(outPortExpr->checkedTo<IR::Literal>());
    else
        return boost::none;

    const auto* outPacketOrigExpr = executionState.get().getPacketBuffer();

    const IR::Expression* outPacketExpr = outPacketOrigExpr;
    if (dynamic_cast<const IR::Concat*>(outPacketOrigExpr) != nullptr) {
        const auto *concat = outPacketOrigExpr->checkedTo<IR::Concat>();
        outPacketExpr = Utils::removeUnknownVar(concat);
    }

    executionState.get().setZeroCksum(Utils::getZeroCksum(outPacketExpr, 0, true));

    auto concolicResolver = VisitConcolicResolver(model,
            executionState.get(), *programInfo.getConcolicMethodImpls());
    outPacketExpr->apply(concolicResolver);

    for (const auto *assert : executionState.get().getPathConstraint()) {
        CHECK_NULL(assert);
        assert->apply(concolicResolver);
    }
    const ConcolicVariableMap *resolvedConcolicVariables =
        concolicResolver.getResolvedConcolicVariables();

    auto concolicOptState = finalState->computeConcolicState(*resolvedConcolicVariables);
    auto replacedState = concolicOptState.value().get();
    const auto *newExecState = replacedState.getExecutionState();
    outPacketExpr = newExecState->getPacketBuffer();
    if (dynamic_cast<const IR::Concat*>(outPacketExpr) != nullptr) {
        const auto *concat = outPacketExpr->checkedTo<IR::Concat>();
        outPacketExpr = Utils::removeUnknownVar(concat);
    }


    const auto &finalModel = replacedState.getFinalModel();
    const auto* outPacket = finalModel.evaluate(outPacketExpr, true);

    const auto* outEvalMask = Taint::buildTaintMask(&finalModel, outPacketExpr);
            //newExecState->getSymbolicEnv().getInternalMap(),

    return Packet(outPortInt, outPacket, outEvalMask);
}

}  // namespace P4Tools::P4Testgen
