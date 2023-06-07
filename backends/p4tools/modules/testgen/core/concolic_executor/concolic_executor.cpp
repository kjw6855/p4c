#include "backends/p4tools/modules/testgen/core/concolic_executor/concolic_executor.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <boost/variant/apply_visitor.hpp>
#include <boost/variant/static_visitor.hpp>

#include "backends/p4tools/common/lib/taint.h"
#include "ir/ir.h"
#include "ir/irutils.h"
#include "ir/visitor.h"
#include "ir/node.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/null.h"
#include "lib/timer.h"
#include "midend/coverage.h"

#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/lib/test_spec.h"

namespace P4Tools {

namespace P4Testgen {

void ConcolicExecutor::run(const TestCase& testCase) {
    executionState = new ExecutionState(programInfo.program);

    while (!executionState->isTerminal()) {

        std::cout << " stack/body size: " << executionState->getStackSize() << "/" << executionState->getBodySize() << std::endl;
        Result successors = evaluator.step(*executionState, testCase);

        if (successors->size() == 1) {
            // Non-branching states are not recorded by selected branches.
            executionState = &(*successors)[0].nextState.get();
            continue;
        }
        // If there are multiple, pop one branch decision from the input list and pick
        // successor matching the given branch decision.
        ExecutionState* next = chooseBranch(*successors, 0);
        if (next == nullptr) {
            break;
        }
        executionState = next;
    }

    if (executionState->isTerminal()) {
        // We've reached the end of the program. Call back and (if desired) end execution.
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

ConcolicExecutor::ConcolicExecutor(const ProgramInfo& programInfo)
    : programInfo(programInfo),
      allStatements(programInfo.getCoverableNodes()),
      evaluator(programInfo),
      statementBitmapSize(allStatements.size()) {

    int allocLen = (statementBitmapSize / 8) + 1;
    statementBitmap = (unsigned char *)malloc(allocLen);
    memset(statementBitmap, 0, allocLen);
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
        std::cout << "Branch Constraint: " << constraint << std::endl;

        if (dynamic_cast<const IR::BoolLiteral*>(constraint) != nullptr) {
            auto val = constraint->checkedTo<IR::BoolLiteral>()->value;
            if (val) {
                next = &branch.nextState.get();
                break;
            }

        } else if (dynamic_cast<const IR::Neq*>(constraint) != nullptr) {
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

bool ConcolicExecutor::testHandleTerminalState(ExecutionState *terminalState) {
    int i = 0;
    auto& visitedStmtSet = terminalState->getVisited();
    for (auto& stmt : allStatements) {
        if (std::count(visitedStmtSet.begin(), visitedStmtSet.end(), stmt) != 0U) {
            int idx = i / 8;
            int shl = 7 - (i % 8);
            statementBitmap[idx] |= 1 << shl;
        }

        i++;
    }

    finalState = new FinalVisitState(*terminalState);

    return true;
}

const std::string ConcolicExecutor::getStatementBitmapStr() {
    int allocLen = (statementBitmapSize / 8) + 1;
    return std::string(reinterpret_cast<char*>(statementBitmap), allocLen);
}

const P4::Coverage::CoverageSet& ConcolicExecutor::getVisitedStatements() {
    return visitedStatements;
}

boost::optional<Packet> ConcolicExecutor::getOutputPacket() {
    if (executionState->getProperty<bool>("drop"))
        return boost::none;

    BUG_CHECK(finalState, "Un-initialized");
    const auto* model = finalState->getCompletedModel();

    const auto* outPortExpr = executionState->get(programInfo.getTargetOutputPortVar());
    int outPortInt = 0;
    if (dynamic_cast<const IR::Literal*>(outPortExpr) != nullptr)
        outPortInt = IR::getIntFromLiteral(outPortExpr->checkedTo<IR::Literal>());
    else
        return boost::none;

    const auto* outPacketExpr = executionState->getPacketBuffer();

#if 0
    /* TODO */
    auto concolicResolver =
        VisitConcolicResolver(model, *executionState, programInfo.getVisitConcolicMethodImpls());
    outPacketExpr->apply(concolicResolver);
    const auto* outPacket = model->evaluate(outPacketExpr);

    const auto* outEvalMask = Taint::buildTaintMask(executionState->getSymbolicEnv().getInternalMap(),
                                                    model, outPacketExpr);

    return Packet(outPortInt, outPacket, outEvalMask);
#endif

    return boost::none;
}

}  // namespace P4Testgen

}  // namespace P4Tools
