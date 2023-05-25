#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/selected_test.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <boost/variant/apply_visitor.hpp>
#include <boost/variant/static_visitor.hpp>

#include "backends/p4tools/common/lib/timer.h"
#include "backends/p4tools/common/lib/taint.h"
#include "gsl/gsl-lite.hpp"
#include "ir/ir.h"
#include "ir/irutils.h"
#include "ir/visitor.h"
#include "ir/node.h"
#include "lib/error.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/null.h"

#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/exploration_strategy.h"
#include "backends/p4tools/modules/fuzzer/core/small_step/visit_stepper.h"
#include "backends/p4tools/modules/fuzzer/core/program_info.h"
#include "backends/p4tools/modules/fuzzer/lib/visit_state.h"
#include "backends/p4tools/modules/fuzzer/lib/test_spec.h"

namespace P4Tools {

namespace P4Testgen {

void SelectedTest::run(const TestCase& testCase) {
    executionState = new VisitState(programInfo.program);

    while (!executionState->isTerminal()) {

        std::cout << " stack/body size: " << executionState->getStackSize() << "/" << executionState->getBodySize() << std::endl;
        VisitResult successors = evaluator.step(*executionState, testCase);

        /*
        if (const auto cmdOpt = executionState->getNextCmd()) {
            struct CmdLogger : public boost::static_visitor<> {
             private:
                SelectedTest& self;
                VisitResult& result;

             public:
                void operator()(const IR::Node* node) {

                    std::cout << "[" << node->node_type_name() << "]";
                    if (dynamic_cast<const IR::P4Program*>(node) == nullptr &&
                            dynamic_cast<const IR::P4Control*>(node) == nullptr) {
                        std::cout << " " << node;
                    }


                    if (result->size() > 1) {
                        std::cout << " (" << result->size() << " branches)";
                    }

                    std::cout << std::endl;
                }
                void operator()(const TraceEvent* event) {}
                void operator()(Continuation::Return ret) {}
                void operator()(Continuation::Exception e) {}
                void operator()(const Continuation::PropertyUpdate& e) {}
                void operator()(const Continuation::Guard& guard) {}

                explicit CmdLogger(SelectedTest& self, VisitResult& result)
                    : self(self), result(result) {}
            } cmdLogger(*this, successors);

            boost::apply_visitor(cmdLogger, *cmdOpt);
        }
        */

        if (successors->size() == 1) {
            // Non-branching states are not recorded by selected branches.
            executionState = (*successors)[0].nextState;
            continue;
        }
        // If there are multiple, pop one branch decision from the input list and pick
        // successor matching the given branch decision.
        VisitState* next = chooseVisitBranch(*successors, 0);
        if (next == nullptr) {
            break;
        }
        executionState = next;
    }

    if (executionState->isTerminal()) {
        // We've reached the end of the program. Call back and (if desired) end execution.
        testHandleTerminalState(*executionState);
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

SelectedTest::SelectedTest(const ProgramInfo& programInfo)
    : programInfo(programInfo),
      allStatements(programInfo.getAllStatements()),
      evaluator(programInfo),
      statementBitmapSize(allStatements.size()) {

    int allocLen = (statementBitmapSize / 8) + 1;
    statementBitmap = (unsigned char *)malloc(allocLen);
    memset(statementBitmap, 0, allocLen);
    //reachabilityEngine = new ReachabilityEngine(programInfo.dcg, "", true);
}

SelectedTest::~SelectedTest() {
    free(statementBitmap);
}

VisitState* SelectedTest::chooseVisitBranch(const std::vector<VisitBranch>& branches,
                                               uint64_t nextVisitBranch) {
    VisitState* next = nullptr;
    for (const auto& branch : branches) {
        const Constraint* constraint = branch.constraint;
        std::cout << "Branch Constraint: " << constraint << std::endl;

        if (dynamic_cast<const IR::BoolLiteral*>(constraint) != nullptr) {
            auto val = constraint->checkedTo<IR::BoolLiteral>()->value;
            if (val) {
                next = branch.nextState;
                break;
            }

        } else if (dynamic_cast<const IR::Neq*>(constraint) != nullptr) {
            // Select the branch temporarily
            next = branch.nextState;
        }
    }

    if (!next) {
        // If not found, the input selected branch list is invalid.
        ::error("The selected branches string doesn't match any branch.");
    }

    return next;
}

bool SelectedTest::testHandleTerminalState(const VisitState& terminalState) {
    int i = 0;
    auto& visitedStmtSet = terminalState.getVisited();
    for (auto& stmt : allStatements) {
        if (std::count(visitedStmtSet.begin(), visitedStmtSet.end(), stmt) != 0U) {
            int idx = i / 8;
            int shl = 7 - (i % 8);
            statementBitmap[idx] |= 1 << shl;
        }

        i++;
    }

    finalState = new FinalVisitState(terminalState);

    return true;
}

const std::string SelectedTest::getStatementBitmapStr() {
    int allocLen = (statementBitmapSize / 8) + 1;
    return std::string(reinterpret_cast<char*>(statementBitmap), allocLen);
}

const P4::Coverage::CoverageSet& SelectedTest::getVisitedStatements() {
    return visitedStatements;
}

boost::optional<Packet> SelectedTest::getOutputPacket() {
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
    auto concolicResolver =
        VisitConcolicResolver(model, *executionState, programInfo.getVisitConcolicMethodImpls());
    outPacketExpr->apply(concolicResolver);
    const auto* outPacket = model->evaluate(outPacketExpr);

    const auto* outEvalMask = Taint::buildTaintMask(executionState->getSymbolicEnv().getInternalMap(),
                                                    model, outPacketExpr);

    return Packet(outPortInt, outPacket, outEvalMask);
}

}  // namespace P4Testgen

}  // namespace P4Tools
