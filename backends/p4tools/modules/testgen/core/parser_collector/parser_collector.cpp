#include "backends/p4tools/modules/testgen/core/parser_collector/parser_collector.h"

#include "backends/p4tools/modules/testgen/core/small_parse_get/small_parse_get.h"

namespace P4Tools::P4Testgen {

/*
std::vector<P4ParserGraphNode>& ParserCollector::getAllNodes() {
    return nodeList;
}

std::vector<P4ParserGraphEdge>& ParserCollector::getAllEdges() {
    return edgeList;
}
*/

std::optional<ExecutionStateReference> ParserCollector::pickSuccessor(Result successors) {
    if (successors->empty()) {
        return std::nullopt;
    }

    // If there is only one successor, choose it and move on.
    if (successors->size() == 1) {
        return successors->at(0).nextState;
    }

    // If there are multiple successors, try to pick one.
    auto newState = successors->back().nextState;
    successors->pop_back();
    // Add the remaining tests to the unexplored branches. Consume the remainder.
    unexploredBranches.insert(unexploredBranches.end(), make_move_iterator(successors->begin()),
                              make_move_iterator(successors->end()));
    return newState;
}

void ParserCollector::handleTerminalState(const ExecutionState &terminalState) {
    const auto &constraints = terminalState.getPathConstraint();

    for (const *constraint : constraints) {
        LOG_FEATURE("",);
    }
}

void ParserCollector::run() {
    // Run small_parse_get

    while (true) {
        if (executionState.get().isTerminal()) {
            handleTerminalState(executionState);

        } else {

            // Result successors = evaluator.parse_get(executionState, parserGraph);
            Result successors = evaluator.parse_get(executionState);
            auto nextState = pickSuccessor(successors);
            if (nextState.has_value()) {
                executionState = nextState.value();
                continue;
            }
        }

        if (unexploredBranches.empty())
            return;

        executionState = unexploredBranches.back().nextState;
        unexploredBranches.pop_back();
    }
}

ParserCollector::ParserCollector(const ProgramInfo &programInfo, const IR::ToplevelBlock *top,
        P4::ReferenceMap *refMap, P4::TypeMap *typeMap, P4ParserGraph *parserGraph)
    : programInfo(programInfo),
      top(top),
      refMap(refMap),
      typeMap(typeMap),
      parserGraph(parserGraph),
      executionState(ExecutionState::create(programInfo.program)),
      evaluator(programInfo) {}

}  // namespace P4Tools::P4Testgen
