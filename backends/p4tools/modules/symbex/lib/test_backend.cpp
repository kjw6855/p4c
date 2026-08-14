#include "backends/p4tools/modules/symbex/lib/test_backend.h"

#include <optional>
#include <sstream>
#include <string>
#include <variant>

#include "backends/p4tools/common/core/z3_solver.h"
#include "backends/p4tools/common/lib/format_int.h"
#include "backends/p4tools/common/lib/model.h"
#include "backends/p4tools/common/lib/taint.h"
#include "backends/p4tools/common/lib/trace_event.h"
#include "backends/p4tools/common/lib/util.h"
#include "ir/irutils.h"
#include "ir/solver.h"
#include "lib/compile_context.h"
#include "lib/cstring.h"
#include "lib/gc.h"
#include "lib/exceptions.h"
#include "lib/null.h"
#include "lib/timer.h"
#include "midend/coverage.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/symbex/lib/concolic.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/lib/final_state.h"
#include "backends/p4tools/modules/symbex/lib/logging.h"
#include "backends/p4tools/modules/symbex/lib/packet_vars.h"
#include "backends/p4tools/modules/symbex/lib/test_framework.h"
#include "backends/p4tools/modules/symbex/options.h"

namespace P4::P4Tools::Symbex {

namespace {

/// Filters ONE expected diagnostic out of a scope, and replays everything else.
///
/// Used around a *speculative* solver query, where "unsatisfiable" is an outcome the caller
/// recovers from and printing it would announce a failure that did not happen. Everything else the
/// scope produced — a solver timeout, a translation failure, any internal error — is written back
/// to the real diagnostic stream on exit, because those are not outcomes anybody recovers from and
/// hiding them would turn a genuine fault into a silently missing test.
///
/// The filter is line-based, matching the shape of what it suppresses: the query's own
/// `warning("... unsatisfiable.")` is a single line with no source context attached. Error and
/// warning COUNTS are untouched — only the text ever goes through this stream — so nothing
/// downstream mistakes a suppressed line for a clean compile.
class DiagnosticFilter {
 public:
    explicit DiagnosticFilter(std::string expectedSubstring)
        : reporter(BaseCompileContext::get().errorReporter()),
          savedStream(reporter.getOutputStream()),
          expected(std::move(expectedSubstring)) {
        reporter.setOutputStream(&sink);
    }
    ~DiagnosticFilter() {
        reporter.setOutputStream(savedStream);
        if (savedStream == nullptr) {
            return;
        }
        std::istringstream captured(sink.str());
        std::string line;
        while (std::getline(captured, line)) {
            if (line.find(expected) != std::string::npos) {
                continue;
            }
            if (line.find_first_not_of(" \t\r") == std::string::npos) {
                continue;
            }
            *savedStream << line << std::endl;
        }
    }
    DiagnosticFilter(const DiagnosticFilter &) = delete;
    DiagnosticFilter(DiagnosticFilter &&) = delete;
    DiagnosticFilter &operator=(const DiagnosticFilter &) = delete;
    DiagnosticFilter &operator=(DiagnosticFilter &&) = delete;

 private:
    ErrorReporter &reporter;
    std::ostream *savedStream;
    std::string expected;
    std::stringstream sink;
};

/// True when two emitted rules of the same table describe the SAME installed entry: identical match
/// values on every key, at the same priority. The control plane cannot tell such rules apart, so
/// one of them is all the device will ever hold.
///
/// Different KEYS are not a conflict and must not be reported as one -- the tampering generator
/// deliberately drives Phase 2's key away from Phase 1's (buildTableKeyNeqConstraint), and two
/// entries the device distinguishes may legitimately carry different actions and data.
///
/// (state_dependency_track.cpp carries the same predicate over its own record type, for the earlier
/// check on the pre-emission models; neither file exports it, matching how cpNameMatches/sameCpName
/// are already duplicated across those two translation units.)
bool sameInstalledRule(const TableRule &a, const TableRule &b) {
    if (a.getPriority() != b.getPriority()) {
        return false;
    }
    const auto *matches1 = a.getMatches();
    const auto *matches2 = b.getMatches();
    if (matches1 == nullptr || matches2 == nullptr) {
        return matches1 == matches2;
    }
    if (matches1->size() != matches2->size()) {
        return false;
    }
    for (const auto &[keyName, match1] : *matches1) {
        auto it = matches2->find(keyName);
        if (it == matches2->end()) {
            return false;
        }
        // isEqualTo compares the whole match: value for exact, value+mask for ternary,
        // value+prefix for LPM, and a mismatched match kind compares unequal.
        if (!match1->isEqualTo(it->second)) {
            return false;
        }
    }
    return true;
}

/// The action-data half of the emitted check, shared by the keyless (default-action) and the keyed
/// (entry) comparison. @p what names the thing in the diagnostic.
bool emittedActionDataAgrees(const char *what, cstring tableName, const ActionCall *call1,
                             const ActionCall *call2, size_t chainId, size_t subTestId) {
    const auto *args1 = call1->getArgs();
    const auto *args2 = call2->getArgs();
    if (args1 == nullptr || args2 == nullptr) {
        return true;
    }
    for (const auto &arg1 : *args1) {
        for (const auto &arg2 : *args2) {
            if (arg1.getActionParamName() != arg2.getActionParamName()) {
                continue;
            }
            if (arg1.getEvaluatedValue()->value == arg2.getEvaluatedValue()->value) {
                break;
            }
            printInfo(
                "[Tampering] chain id=%1% sub=%2%: table '%3%' would need %4% %5%(%6%) = %7% for "
                "Phase 1 and %8% for Phase 2; the control plane installs one value for both and no "
                "annotation fixes it, so this test is unrealizable — dropping.",
                chainId, subTestId, tableName, what, call1->getActionName(),
                arg1.getActionParamName(), arg1.getEvaluatedValue()->value,
                arg2.getEvaluatedValue()->value);
            return false;
        }
    }
    return true;
}

/// Cross-phase control-plane consistency on the EMITTED specs.
///
/// The hardware installs ONE control-plane configuration for the whole replay -- one default action
/// per table, and one entry per match key -- but each phase is re-solved by its own processPhase
/// call, so an action-data symbol that neither phase's path nails down is re-rolled independently:
/// the generator can see the phases agree and the emitted models still come out different. The
/// emitters then merge both phases' rules into one `entities` list (bfrt.cpp's collectRules and its
/// bmv2 counterpart), so two rules that share a match key and disagree on their data end up as two
/// installs of one entry -- the second either fails or silently overwrites the first, and whichever
/// phase depended on the losing value no longer behaves as symbex predicted.
///
/// Action data nobody constrained is not something p4symbex may invent a value for -- only a
/// --cp-annotation `action_data` clause states what the controller installs, and such a clause pins
/// every phase to the same literal (TableStepper::cpActionArgPin re-applies it inside every phase's
/// own query), so it never trips this check. Everything else fails closed: a test whose two packets
/// need different action data for the same installed entry describes a device configuration that
/// cannot exist, and is dropped rather than emitted with two contradictory values.
///
/// Names are compared with ==: both specs' table labels come from IR::P4Table::controlPlaneName()
/// and both action names from IR::P4Action::controlPlaneName(), so they are spelled identically.
bool emittedControlPlaneAgrees(const TestSpec *spec1, const TestSpec *spec2, size_t chainId,
                               size_t subTestId) {
    if (!SymbexOptions::get().crossPhaseDefaultActionPin) {
        return true;
    }
    CHECK_NULL(spec1);
    CHECK_NULL(spec2);
    const auto tables2 = spec2->getTestObjectCategory("tables"_cs);
    for (const auto &[tableName, obj1] : spec1->getTestObjectCategory("tables"_cs)) {
        const auto *cfg1 = obj1->to<TableConfig>();
        if (cfg1 == nullptr) {
            continue;
        }
        auto it = tables2.find(tableName);
        if (it == tables2.end()) {
            continue;
        }
        const auto *cfg2 = it->second->to<TableConfig>();
        if (cfg2 == nullptr) {
            continue;
        }
        // Keyed entries. Matched pairwise by match value rather than by position: the two phases
        // build their rule lists independently, so equal indices mean nothing.
        for (const auto &rule1 : *cfg1->getRules()) {
            for (const auto &rule2 : *cfg2->getRules()) {
                if (!sameInstalledRule(rule1, rule2)) {
                    continue;
                }
                const auto *entryCall1 = rule1.getActionCall();
                const auto *entryCall2 = rule2.getActionCall();
                if (entryCall1 == nullptr || entryCall2 == nullptr) {
                    continue;
                }
                if (entryCall1->getActionName() != entryCall2->getActionName()) {
                    printInfo(
                        "[Tampering] chain id=%1% sub=%2%: table '%3%' would need action '%4%' for "
                        "Phase 1 and '%5%' for Phase 2 behind the same match key; one entry cannot "
                        "run two actions, so this test is unrealizable — dropping.",
                        chainId, subTestId, tableName, entryCall1->getActionName(),
                        entryCall2->getActionName());
                    return false;
                }
                if (!emittedActionDataAgrees("action data", tableName, entryCall1, entryCall2,
                                             chainId, subTestId)) {
                    return false;
                }
            }
        }
        // Default-action override (the keyless path, and any keyed table whose phase took the
        // default): a table property rather than a rule.
        const auto *prop1 = cfg1->getProperty("overriden_default_action"_cs, /*checked=*/false);
        if (prop1 == nullptr) {
            continue;
        }
        const auto *prop2 = cfg2->getProperty("overriden_default_action"_cs, /*checked=*/false);
        if (prop2 == nullptr) {
            continue;
        }
        const auto *call1 = prop1->to<ActionCall>();
        const auto *call2 = prop2->to<ActionCall>();
        if (call1 == nullptr || call2 == nullptr) {
            continue;
        }
        if (call1->getActionName() != call2->getActionName()) {
            printInfo(
                "[Tampering] chain id=%1% sub=%2%: table '%3%' would need default action '%4%' for "
                "Phase 1 and '%5%' for Phase 2; the control plane installs one action for both, so "
                "this test is unrealizable — dropping.",
                chainId, subTestId, tableName, call1->getActionName(), call2->getActionName());
            return false;
        }
        if (!emittedActionDataAgrees("default action data", tableName, call1, call2, chainId,
                                     subTestId)) {
            return false;
        }
    }
    return true;
}

}  // namespace

TestBackEnd::TestBackEnd(const ProgramInfo &programInfo,
                         const TestBackendConfiguration &testBackendConfiguration,
                         SymbolicExecutor &symbex)
    : programInfo(programInfo),
      testBackendConfiguration(testBackendConfiguration),
      symbex(symbex),
      maxTests(SymbexOptions::get().maxTests) {
    // If we select a specific branch, the number of tests should be 1.
    if (!SymbexOptions::get().selectedBranches.empty()) {
        maxTests = 1;
    }
}

bool TestBackEnd::run(const FinalState &state) {
    {
        // Evaluate the model and extract the input and output packets.
        const auto *executionState = state.getExecutionState();
        const auto *outputPacketExpr = executionState->getPacketBuffer();
        const auto *outputPortExpr = executionState->get(getProgramInfo().getTargetOutputPortVar());
        const auto &coverableNodes = getProgramInfo().getCoverableNodes();
        const auto *programTraces = state.getTraces();
        const auto &symbexOptions = SymbexOptions::get();

        // Don't increase the test count if --output-packet-only is enabled and we don't
        // produce a test with an output packet.
        if (symbexOptions.outputPacketOnly) {
            if (executionState->getPacketBufferSize() <= 0 ||
                executionState->getProperty<bool>("drop"_cs)) {
                return needsToTerminate(testCount);
            }
        }

        // Don't increase the test count if --dropped-packet-only is enabled and we produce a test
        // with an output packet.
        if (symbexOptions.droppedPacketOnly) {
            if (!executionState->getProperty<bool>("drop"_cs)) {
                return needsToTerminate(testCount);
            }
        }

        // If assertion mode is active, ignore any test that does not trigger an assertion.
        if (symbexOptions.assertionModeEnabled) {
            if (!executionState->getProperty<bool>("assertionTriggered"_cs)) {
                return needsToTerminate(testCount);
            }
            printInfo("AssertionMode: Found an input that triggers an assertion.");
        }

        // For long-running tests periodically reset the solver state to free up memory.
        if (testCount != 0 && testCount % RESET_THRESHOLD == 0) {
            auto &solver = state.getSolver();
            auto *z3Solver = solver.to<Z3Solver>();
            CHECK_NULL(z3Solver);
            z3Solver->clearMemory();
        }

        bool abort = false;

        // Execute concolic functions that may occur in the output packet, the output port,
        // or any path conditions.
        auto concolicResolver = ConcolicResolver(state.getFinalModel(), *executionState,
                                                 *getProgramInfo().getConcolicMethodImpls());

        outputPacketExpr->apply(concolicResolver);
        outputPortExpr->apply(concolicResolver);
        for (const auto *assert : executionState->getPathConstraint()) {
            CHECK_NULL(assert);
            assert->apply(concolicResolver);
        }
        const ConcolicVariableMap *resolvedConcolicVariables =
            concolicResolver.getResolvedConcolicVariables();
        // If we resolved concolic variables and substitute them, check the solver again under
        // the new constraints.
        auto concolicOptState = state.computeConcolicState(*resolvedConcolicVariables);
        if (!concolicOptState.has_value()) {
            testCount++;
            return needsToTerminate(testCount);
        }
        auto replacedState = concolicOptState.value().get();
        executionState = replacedState.getExecutionState();
        outputPacketExpr = executionState->getPacketBuffer();
        const auto &finalModel = replacedState.getFinalModel();
        outputPortExpr = executionState->get(getProgramInfo().getTargetOutputPortVar());

        auto testInfo = produceTestInfo(executionState, &finalModel, outputPacketExpr,
                                        outputPortExpr, programTraces);

        // Add a list of tracked branches to the test output, too.
        std::stringstream selectedBranches;
        if (symbexOptions.trackBranches) {
            symbex.printCurrentTraceAndBranches(selectedBranches, *executionState);
        }

        abort = printTestInfo(executionState, testInfo, outputPortExpr);
        if (abort) {
            testCount++;
            return needsToTerminate(testCount);
        }
        const auto *testSpec = createTestSpec(executionState, &finalModel, testInfo);

        // Commit an update to the visited nodes.
        // Only do this once we are sure we are generating a test.
        auto hasUpdated = symbex.updateVisitedNodes(replacedState.getVisited());

        // Skip test case generation if the --only-covering-tests is enabled and we do not increase
        // coverage.
        if (!coverableNodes.empty() && symbexOptions.coverageOptions.onlyCoveringTests &&
            !hasUpdated) {
            return needsToTerminate(testCount);
        }

        testCount++;
        const P4::Coverage::CoverageSet &visitedNodes = symbex.getVisitedNodes();
        int testCoverage = 0;
        int bitmapSize = 0;
        unsigned char *testCoverageMap = nullptr;
        if (!symbexOptions.hasCoverageTracking) {
            printInfo("============ Test %1% ============", testCount);
        } else if (coverableNodes.empty()) {
            printInfo("============ Test %1%: No coverable nodes ============", testCount);
            // All 0 nodes covered.
            coverage = 1.0;
        } else {
            bitmapSize = coverableNodes.size();
            int allocLen = (bitmapSize / 8) + 1;
            testCoverageMap = (unsigned char *)malloc(allocLen);
            memset(testCoverageMap, 0, allocLen);
            int i = 0;
            auto& visitedStmtSet = executionState->getVisited();
            LOG_FEATURE("coverage", 5, "visited: " << visitedStmtSet.size());
            for (auto *node : coverableNodes) {
                if (visitedStmtSet.count(node)) {
                    int idx = i / 8;
                    int shl = 7 - (i % 8);
                    testCoverageMap[idx] |= 1 << shl;
                    testCoverage ++;
                }

                i++;
            }

            coverage =
                static_cast<float>(visitedNodes.size()) / static_cast<float>(coverableNodes.size());
            printInfo("============ Test %1%: Nodes covered: %2% (%3% .. %4%/%5%) ============",
                    testCount, coverage, testCoverage,
                    visitedNodes.size(), coverableNodes.size());
            P4::Coverage::logCoverage(coverableNodes, visitedNodes, visitedStmtSet);
        }

        // Output the test.
        Util::withTimer("backend", [this, &testSpec, &selectedBranches, testCoverageMap, bitmapSize] {
            if (testWriter->isInFileMode()) {
                testWriter->writeTestToFile(testSpec, selectedBranches, testCount, coverage, testCoverageMap, bitmapSize);
            } else {
                auto testOpt =
                    testWriter->produceTest(testSpec, selectedBranches, testCount, coverage, testCoverageMap, bitmapSize);
                if (!testOpt.has_value()) {
                    BUG("Failed to produce test.");
                }
                tests.push_back(testOpt.value());
            }
        });

        if (testCoverageMap)
            free(testCoverageMap);

        printTraces("============ End Test %1% ============\n", testCount);
        P4::Coverage::printCoverageReport(coverableNodes, visitedNodes);

        // If MAX_NODE_COVERAGE is enabled, terminate early if we hit max node coverage already.
        if (symbexOptions.stopMetric == "MAX_NODE_COVERAGE" && coverage == 1.0) {
            return true;
        }
#ifdef SYMBEX_PRINT_PERFORMANCE_PER_TEST
        printPerformanceReport(std::nullopt);
#endif
        return needsToTerminate(testCount);
    }
}

TestBackEnd::TestInfo TestBackEnd::produceTestInfo(
    const ExecutionState *executionState, const Model *finalModel,
    const IR::Expression *outputPacketExpr, const IR::Expression *outputPortExpr,
    const std::vector<std::reference_wrapper<const TraceEvent>> *programTraces) {
    // Evaluate all the important expressions necessary for program execution by using the
    // final model.
    int calculatedPacketSize =
        IR::getIntFromLiteral(finalModel->evaluate(ExecutionState::getInputPacketSizeVar(), true));
    const auto *inputPacketExpr = executionState->getInputPacket();
    // The payload fills the space between the minimum input size needed and the symbolically
    // calculated packet size.
    const auto *payloadExpr = finalModel->get(&PacketVars::PAYLOAD_SYMBOL, false);
    if (payloadExpr != nullptr) {
        inputPacketExpr =
            new IR::Concat(IR::Type_Bits::get(calculatedPacketSize), inputPacketExpr, payloadExpr);
        outputPacketExpr = new IR::Concat(IR::Type_Bits::get(outputPacketExpr->type->width_bits() +
                                                             payloadExpr->type->width_bits()),
                                          outputPacketExpr, payloadExpr);
    }
    const auto *inputPacket = finalModel->evaluate(inputPacketExpr, true);
    const auto *outputPacket = finalModel->evaluate(outputPacketExpr, true);
    const auto *inputPort =
        finalModel->evaluate(executionState->get(getProgramInfo().getTargetInputPortVar()), true);

    const auto *outputPortVar = finalModel->evaluate(outputPortExpr, true);
    // Build the taint mask by dissecting the program packet variable
    const auto *evalMask = Taint::buildTaintMask(finalModel, outputPacketExpr);

    // A tainted (but not UninitializedTaintExpression) egress port is UNKNOWN, not a drop: symbex
    // can't pin it (e.g. a hash-derived port). Emit the reserved sentinel rather than a misleading
    // concretized value; the tampering differential oracle observes the actual egress at replay.
    bool outputPortIsUnknown = Taint::hasTaint(outputPortExpr) &&
                               !outputPortExpr->is<IR::UninitializedTaintExpression>();

    // Get the input/output port integers.
    auto inputPortInt = IR::getIntFromLiteral(inputPort);
    auto outputPortInt = outputPortIsUnknown ? SYMBEX_UNKNOWN_PORT : IR::getIntFromLiteral(outputPortVar);

    // A propagated-tainted egress is "unknown — decided at replay", NOT a drop. Some targets (tna's
    // check_tofino_drop) conservatively mark such a packet dropped; override that here so the test
    // emits the egress packet with the unknown sentinel port instead of recording a drop. Genuine
    // drops (UninitializedTaintExpression, drop_ctl, egress_spec==DROP_PORT) have
    // outputPortIsUnknown=false and are unaffected.
    bool dropped = executionState->getProperty<bool>("drop"_cs) && !outputPortIsUnknown;

    return {inputPacket->checkedTo<IR::Constant>(),  inputPortInt,
            outputPacket->checkedTo<IR::Constant>(), outputPortInt,
            evalMask->checkedTo<IR::Constant>(),     *programTraces,
            dropped,                                 outputPortIsUnknown};
}

bool TestBackEnd::printTestInfo(const ExecutionState * /*executionState*/, const TestInfo &testInfo,
                                const IR::Expression *outputPortExpr) {
    // Print all the important variables and properties of this test.
    printTraces("============ Program trace for Test %1% ============\n", testCount);
    for (const auto &event : testInfo.programTraces) {
        printTraces("%1%", event);
    }

    auto inputPacketSize = testInfo.inputPacket->type->width_bits();
    auto outputPacketSize = testInfo.outputPacket->type->width_bits();

    printTraces("=======================================");
    printTraces("============ Input packet for Test %1% ============", testCount);
    printTraces(formatHexExpr(testInfo.inputPacket, {false, true, false}));
    printTraces("=======================================");
    // We have no control over the test, if the output port is tainted. So we abort.
    if (Taint::hasTaint(outputPortExpr)) {
        printInfo("============ Test %1%: Output port tainted - Aborting Test ============",
                  testCount);
        return true;
    }
    printTraces("Input packet size: %1%", inputPacketSize);
    printTraces("============ Ports for Test %1% ============", testCount);
    printTraces("Input port: %1%", testInfo.inputPort);
    printTraces("=======================================");

    if (testInfo.packetIsDropped) {
        printTraces("============ Output packet dropped for Test %1% ============", testCount);
        return false;
    }

    BUG_CHECK(outputPacketSize >= 0, "Invalid out packet size (%1% bits) calculated!",
              outputPacketSize);
    printTraces("============ Output packet for Test %1% ============", testCount);
    printTraces(formatHexExpr(testInfo.outputPacket, {false, true, false}));
    printTraces("=======================================");
    printTraces("Output packet size: %1% ", outputPacketSize);
    printTraces("=======================================");
    printTraces("============ Output mask Test %1% ============", testCount);
    printTraces(formatHexExpr(testInfo.packetTaintMask, {false, true, false}));
    printTraces("=======================================");
    printTraces("Output port: %1%\n", testInfo.outputPort);
    printTraces("=======================================");

    return false;
}

std::optional<TestBackEnd::PhaseResult> TestBackEnd::processPhase(
    const FinalState &state, std::optional<int> overrideInputPort,
    std::optional<int> overrideOutputPort,
    const std::vector<std::pair<const IR::SymbolicVariable *, const IR::Constant *>>
        &modelOverrides,
    const std::vector<const IR::Expression *> &extraConstraints, bool allowTaintedOutput) {
    const auto *executionState = state.getExecutionState();
    const auto *outputPacketExpr = executionState->getPacketBuffer();
    const auto *outputPortExpr = executionState->get(getProgramInfo().getTargetOutputPortVar());
    const auto *programTraces = state.getTraces();

    auto concolicResolver = ConcolicResolver(state.getFinalModel(), *executionState,
                                             *getProgramInfo().getConcolicMethodImpls());
    outputPacketExpr->apply(concolicResolver);
    outputPortExpr->apply(concolicResolver);
    for (const auto *assert : executionState->getPathConstraint()) {
        CHECK_NULL(assert);
        assert->apply(concolicResolver);
    }
    // Snapshot of the assert set as it stood before the index pin below: everything the emitted
    // packet, the ports and the path already depend on. computeConcolicState turns every resolved
    // concolic variable into an assert, so pinning the index can only ever *add* constraints --
    // this snapshot is the query we re-solve with when it turns out to add too many.
    const ConcolicVariableMap baseConcolicVariables =
        *concolicResolver.getResolvedConcolicVariables();

    // The register access index expressions. Nothing above necessarily reads them -- a program that
    // hashes into a sketch but never branches on the bucket number does not -- so without visiting
    // them the index-producing concolic method is never executed and the model reports Z3's default
    // for its label, i.e. cell 0 for every emitted test.
    std::vector<const IR::Expression *> indexExpressions;
    for (const auto &[registerName, registerObject] :
         executionState->getTestObjectCategory("registervalues"_cs)) {
        for (const auto *indexExpr : registerObject->getIndexExpressions()) {
            // A tainted index (a RANDOM hash, say) has no value to resolve; the tampering generator
            // pins the whole attacker packet to Phase 1 for those instead.
            if (indexExpr == nullptr || Taint::hasTaint(indexExpr)) {
                continue;
            }
            indexExpressions.push_back(indexExpr);
        }
    }
    for (const auto *indexExpr : indexExpressions) {
        indexExpr->apply(concolicResolver);
    }
    const ConcolicVariableMap *pinnedConcolicVariables =
        concolicResolver.getResolvedConcolicVariables();
    // No new entry means the indices hold no concolic method the base query did not already assert
    // -- a plain header field as index, or a hash the path itself branched on. The model binds those
    // either way, so there is nothing to enforce and nothing to report.
    const bool indexAddsAsserts = pinnedConcolicVariables->size() > baseConcolicVariables.size();

    /// The concrete cells @p fs's model addresses, one entry per indexExpressions entry, with the
    /// index-producing concolic implementations run against that model first. Returns an empty
    /// vector when anything fails to resolve, which callers read as "unknown" -- never as a
    /// mismatch. This is the same resolution the tampering generator performs on this phase's DFS
    /// model when it concretises affected_register.index, so comparing it before and after the
    /// re-solve says whether the emitted packet still addresses the cell that got recorded.
    auto resolveIndexCells = [&](const FinalState &fs) -> std::vector<big_int> {
        std::vector<big_int> cells;
        if (indexExpressions.empty()) {
            return cells;
        }
        try {
            const auto &model = fs.getFinalModel();
            auto indexResolver = ConcolicResolver(model, *fs.getExecutionState(),
                                                  *getProgramInfo().getConcolicMethodImpls());
            for (const auto *indexExpr : indexExpressions) {
                indexExpr->apply(indexResolver);
            }
            Model resolvedModel(model);
            for (const auto &[concolicVariable, assignment] :
                 *indexResolver.getResolvedConcolicVariables()) {
                if (!std::holds_alternative<IR::ConcolicVariable>(concolicVariable)) {
                    continue;
                }
                resolvedModel.set(std::get<IR::ConcolicVariable>(concolicVariable).clone(),
                                  assignment);
            }
            for (const auto *indexExpr : indexExpressions) {
                const auto *folded =
                    resolvedModel.evaluate(indexExpr, /*doComplete=*/true)->to<IR::Constant>();
                if (folded == nullptr) {
                    return {};
                }
                cells.push_back(folded->value);
            }
        } catch (const std::exception &) {
            return {};
        }
        return cells;
    };

    // Build IR::Equ constraints to pin the port values detected in runTamperingScenario.
    // These are passed to computeConcolicState() so Z3 incorporates them into checkSat —
    // the fresh model it produces is therefore guaranteed to satisfy them.
    std::vector<const IR::Expression *> portConstraints;
    if (overrideInputPort.has_value()) {
        const auto *portExpr = executionState->get(getProgramInfo().getTargetInputPortVar());
        portConstraints.push_back(
            new IR::Equ(portExpr, IR::Constant::get(portExpr->type, *overrideInputPort)));
    }
    if (overrideOutputPort.has_value()) {
        portConstraints.push_back(
            new IR::Equ(outputPortExpr, IR::Constant::get(outputPortExpr->type, *overrideOutputPort)));
    }

    // Append caller-supplied NEQ constraints (e.g. Phase 2 key pin-aways for Phase 1 re-solve)
    // so Z3 cannot re-assign Phase 1's packet fields to Phase 2's already-determined key values.
    for (const auto *neq : extraConstraints) {
        portConstraints.push_back(neq);
    }

    // Two-attempt emission for the register index. Attempt 1 ENFORCES it: the extra asserts are not
    // just `hash_label == cell` -- a hash concolic implementation also records every INPUT it read
    // (Hash_get's resolvedExpressions, and the bmv2 equivalent), so the assert set pins the packet
    // fields that feed the hash as well. That is what makes enforcement real: the re-solved packet
    // cannot drift onto a different cell, because the bytes the cell is computed from are fixed.
    //
    // It is only an attempt because a program that branches on hash bits (a count-min sketch on its
    // sign bits, say) has already committed to whatever cell Z3 picked for that branch, and
    // asserting the real hash then contradicts the path. Losing those cases entirely -- the
    // "0 txtpb for hash-indexed sketches" wall -- is far worse than reporting a cell we could not
    // enforce, hence attempt 2 below.
    std::optional<std::reference_wrapper<const FinalState>> concolicOptState;
    if (SymbexOptions::get().pinIndexValue && indexAddsAsserts) {
        // UNSAT is an expected outcome here, not an error -- the fallback recovers from it. A
        // timeout or an internal error is NOT, and still reaches the user.
        DiagnosticFilter quiet("unsatisfiable");
        concolicOptState = state.computeConcolicState(*pinnedConcolicVariables, portConstraints);
    }

    // Attempt 2: re-solve with exactly the pre-index assert set, then resolve the indices against
    // the model that solve produced and *report* them without asserting them, so the register test
    // objects the target backend evaluates out of the emission model also carry a real hash value
    // rather than the unconstrained default. (affected_register.index does not come from here --
    // the tampering generator already folded it, see modelWithResolvedIndices -- so this is about
    // the ordinary per-phase `registervalues` the emitters write alongside it.)
    bool indexReportedNotEnforced = false;
    SymbolicMapping reportedIndexBindings;
    if (!concolicOptState.has_value()) {
        concolicOptState = state.computeConcolicState(baseConcolicVariables, portConstraints);
        if (!concolicOptState.has_value()) {
            return std::nullopt;
        }
        if (indexAddsAsserts) {
            const auto &solvedState = concolicOptState.value().get();
            auto indexResolver = ConcolicResolver(solvedState.getFinalModel(),
                                                  *solvedState.getExecutionState(),
                                                  *getProgramInfo().getConcolicMethodImpls());
            for (const auto *indexExpr : indexExpressions) {
                indexExpr->apply(indexResolver);
            }
            for (const auto &[concolicVariable, assignment] :
                 *indexResolver.getResolvedConcolicVariables()) {
                // Only concolic variables carry a label the model can be keyed by; whole-expression
                // keys have no symbol to bind.
                if (!std::holds_alternative<IR::ConcolicVariable>(concolicVariable)) {
                    continue;
                }
                reportedIndexBindings.emplace(
                    std::get<IR::ConcolicVariable>(concolicVariable).clone(), assignment);
            }
            indexReportedNotEnforced = !reportedIndexBindings.empty();
        }
    } else if (indexAddsAsserts) {
        // Attempt 1 held as a query, but "held" is worth checking rather than assuming. Re-resolve
        // the indices against the model actually being emitted and compare them with the cells this
        // phase's DFS model addressed -- the very cells the tampering generator already wrote into
        // affected_register.index (modelWithResolvedIndices, called where the attacker register
        // object is built). Equal means the emitted packet really does address the recorded cell.
        const auto recordedIndexCells = resolveIndexCells(state);
        const auto emittedIndexCells = resolveIndexCells(concolicOptState.value().get());
        if (!recordedIndexCells.empty() && !emittedIndexCells.empty() &&
            emittedIndexCells != recordedIndexCells) {
            indexReportedNotEnforced = true;
            printInfo(
                "[Tampering] register index pin did not hold: the re-solved packet addresses cell "
                "%1% while the emitted test records cell %2%. Reporting the recorded cell without "
                "claiming it is enforced.",
                emittedIndexCells.front().str(), recordedIndexCells.front().str());
        }
    }

    auto &replacedState = concolicOptState.value().get();
    executionState = replacedState.getExecutionState();
    outputPacketExpr = executionState->getPacketBuffer();
    const auto &finalModel = replacedState.getFinalModel();
    outputPortExpr = executionState->get(getProgramInfo().getTargetOutputPortVar());

    if (Taint::hasTaint(outputPortExpr) && !allowTaintedOutput) {
        return std::nullopt;
    }

    // Apply direct model overrides (attacker-chosen register values for Phase 2).
    // Model::set() overwrites existing entries so these take priority over Z3's assignment.
    const Model *effectiveModel = &finalModel;
    std::optional<Model> overriddenModel;
    if (!modelOverrides.empty() || !reportedIndexBindings.empty()) {
        overriddenModel.emplace(finalModel);
        // Report-only index bindings first, and via mergeMap rather than set(): an index the solver
        // itself bound (because the path branched on it) must keep the value the path chose, or the
        // emitted cell would contradict the branch the packet takes.
        overriddenModel->mergeMap(reportedIndexBindings);
        for (const auto &[var, val] : modelOverrides) {
            overriddenModel->set(var, val);
        }
        effectiveModel = &overriddenModel.value();
    }

    auto testInfo = produceTestInfo(executionState, effectiveModel, outputPacketExpr,
                                    outputPortExpr, programTraces);
    const auto *testSpec = createTestSpec(executionState, effectiveModel, testInfo);
    return PhaseResult{testSpec, testInfo.packetIsDropped, indexReportedNotEnforced};
}

bool TestBackEnd::runTampering(const TamperingFinalState &state) {
    // Pass the pre-computed concrete port numbers as overrides. computeConcolicState()
    // re-solves the SMT model and may assign different (but satisfying) port values;
    // the ports from runTamperingScenario are the ones that satisfy the cross-phase
    // constraints (input1==input3, input2 != input1, input2 != output1, etc.).
    std::optional<int> p1in  = (state.phase1InputPort  >= 0) ? std::optional<int>(state.phase1InputPort)  : std::nullopt;
    std::optional<int> p1out = (state.phase1OutputPort >= 0) ? std::optional<int>(state.phase1OutputPort) : std::nullopt;
    std::optional<int> p2in  = (state.phase2InputPort  >= 0) ? std::optional<int>(state.phase2InputPort)  : std::nullopt;
    std::optional<int> p2out = (state.phase2OutputPort >= 0) ? std::optional<int>(state.phase2OutputPort) : std::nullopt;

    // allowTaintedOutput: tampering tests are validated by the differential oracle (the harness
    // replays and compares the two runs' actual Phase-3 outputs), so a symbex-unknown egress port is
    // acceptable — emit the test and let runtime decide the port.
    auto res1 = processPhase(state.phase1, p1in, p1out, {}, state.phase1ExtraConstraints,
                             /*allowTaintedOutput=*/true);
    if (!res1.has_value()) {
        testCount++;
        return needsToTerminate(testCount);
    }

    auto res2 = processPhase(state.phase2, p2in, p2out, state.phase2ModelOverrides, {},
                             /*allowTaintedOutput=*/true);
    if (!res2.has_value()) {
        testCount++;
        return needsToTerminate(testCount);
    }

    // Both phases have now been re-solved independently, each under its own port overrides and
    // concolic assert set. That re-solve is free to pick a different value for any control-plane
    // action-data symbol the path does not nail down, so the cross-phase agreement the generator
    // established on the pre-emission models is not automatically true of the models being written
    // out. Check it here, on the specs that actually become the test.
    if (!emittedControlPlaneAgrees(res1->testSpec, res2->testSpec, state.chainId,
                                   state.subTestId)) {
        testCount++;
        return needsToTerminate(testCount);
    }

    // Phase 3: HIT→MISS leaves it a dynamic deviation check. MISS→HIT carries a
    // symbolically-verified Phase-3 terminal whose expected output we materialise (phase3_verify)
    // so the emitted test asserts a concrete Phase-1 ≠ Phase-3 output.
    TamperingTestSpec tamperingSpec(res1->testSpec, res2->testSpec,
                                    state.readPathHasExit, state.attackerRegisterValues,
                                    state.attackerRegisterSinkTables);
    tamperingSpec.missToHit = state.missToHit;
    // The case label rides the emitted "Tamper case:" metadata line on every target, so tag a
    // report-only index there: such a test names the cell the attacker packet hashes to in the
    // model, but the solver was never obliged to keep it, and triage must not read it as a pinned
    // cell.
    if (res1->indexReportedNotEnforced || res2->indexReportedNotEnforced) {
        std::string label = state.caseLabel.string();
        if (!label.empty()) {
            label += " ";
        }
        label += "index-reported-not-enforced";
        tamperingSpec.caseLabel = cstring(label);
    } else {
        tamperingSpec.caseLabel = state.caseLabel;
    }
    tamperingSpec.usesMulticast = state.usesMulticast;
    tamperingSpec.multicastGroupId = state.multicastGroupId;
    tamperingSpec.phase2RepeatCount = state.phase2RepeatCount;
    tamperingSpec.attackerRegisterMinValues = state.attackerRegisterMinValues;
    // Phase 3 is a dynamic deviation check for both directions: the test script replays Phase 1's
    // packet and the end-to-end validator compares the Phase-3 output to the Phase-1 reference
    // (detecting drop/port/byte divergence — including non-drop table misses). p4symbex therefore
    // emits no predicted phase3_verify; the sink-flip metadata (hit_phase/miss_phase + sink_table)
    // plus Phase 1's own expected output is all the validator needs.

    // Build selected-branches string from the symbolic executor.
    std::stringstream selectedBranches;
    const auto &symbexOptions = SymbexOptions::get();
    if (symbexOptions.trackBranches) {
        const auto *executionState = state.phase1.getExecutionState();
        symbex.printCurrentTraceAndBranches(selectedBranches,
                                            *executionState);
    }

    testCount++;
    printInfo("============ Tampering Test %1% (chain=%2% sub=%3%) ============",
              testCount, state.chainId, state.subTestId);

    Util::withTimer("backend", [this, &tamperingSpec, &selectedBranches, &state] {
        testWriter->writeTestToFile(&tamperingSpec, selectedBranches, state.chainId,
                                    state.subTestId, coverage);
    });

    printTraces("============ End Tampering Test %1% ============\n", testCount);
    // Per-chain capping in runTamperingScenario governs the total test count, so we never
    // abort the whole run here — returning true would stop emission after the first chain
    // once the global maxTests was reached. testCount is still tracked for logging.
    return false;
}

int64_t TestBackEnd::getTestCount() const { return testCount; }

float TestBackEnd::getCoverage() const { return coverage; }

const ProgramInfo &TestBackEnd::getProgramInfo() const { return programInfo; }

const TestBackendConfiguration &TestBackEnd::getTestBackendConfiguration() const {
    return testBackendConfiguration;
}

bool TestBackEnd::needsToTerminate(int64_t testCount) const {
    // If maxTests is 0, we never "need" to terminate because we want to produce as many tests as
    // possible.
    return maxTests != 0 && testCount >= maxTests;
}
}  // namespace P4::P4Tools::Symbex
