#include "backends/p4tools/modules/symbex/lib/test_backend.h"

#include <optional>

#include "backends/p4tools/common/core/z3_solver.h"
#include "backends/p4tools/common/lib/format_int.h"
#include "backends/p4tools/common/lib/model.h"
#include "backends/p4tools/common/lib/taint.h"
#include "backends/p4tools/common/lib/trace_event.h"
#include "backends/p4tools/common/lib/util.h"
#include "ir/irutils.h"
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

    // Get the input/output port integers.
    auto inputPortInt = IR::getIntFromLiteral(inputPort);
    auto outputPortInt = IR::getIntFromLiteral(outputPortVar);

    return {inputPacket->checkedTo<IR::Constant>(),      inputPortInt,
            outputPacket->checkedTo<IR::Constant>(),     outputPortInt,
            evalMask->checkedTo<IR::Constant>(),         *programTraces,
            executionState->getProperty<bool>("drop"_cs)};
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
    const std::vector<const IR::Expression *> &extraConstraints) {
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
    const ConcolicVariableMap *resolvedConcolicVariables =
        concolicResolver.getResolvedConcolicVariables();

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
    auto concolicOptState = state.computeConcolicState(*resolvedConcolicVariables, portConstraints);
    if (!concolicOptState.has_value()) {
        return std::nullopt;
    }
    auto &replacedState = concolicOptState.value().get();
    executionState = replacedState.getExecutionState();
    outputPacketExpr = executionState->getPacketBuffer();
    const auto &finalModel = replacedState.getFinalModel();
    outputPortExpr = executionState->get(getProgramInfo().getTargetOutputPortVar());

    if (Taint::hasTaint(outputPortExpr)) {
        return std::nullopt;
    }

    // Apply direct model overrides (attacker-chosen register values for Phase 2).
    // Model::set() overwrites existing entries so these take priority over Z3's assignment.
    const Model *effectiveModel = &finalModel;
    std::optional<Model> overriddenModel;
    if (!modelOverrides.empty()) {
        overriddenModel.emplace(finalModel);
        for (const auto &[var, val] : modelOverrides) {
            overriddenModel->set(var, val);
        }
        effectiveModel = &overriddenModel.value();
    }

    auto testInfo = produceTestInfo(executionState, effectiveModel, outputPacketExpr,
                                    outputPortExpr, programTraces);
    const auto *testSpec = createTestSpec(executionState, effectiveModel, testInfo);
    return PhaseResult{testSpec, testInfo.packetIsDropped};
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

    auto res1 = processPhase(state.phase1, p1in, p1out, {}, state.phase1ExtraConstraints);
    if (!res1.has_value()) {
        testCount++;
        return needsToTerminate(testCount);
    }

    auto res2 = processPhase(state.phase2, p2in, p2out, state.phase2ModelOverrides);
    if (!res2.has_value()) {
        testCount++;
        return needsToTerminate(testCount);
    }

    // Phase 3 is purely dynamic: the test script replays Phase 1's packet after Phase 2
    // writes the attacker-chosen value. No symbex is run for Phase 3.
    TamperingTestSpec tamperingSpec(res1->testSpec, res2->testSpec,
                                    state.readPathHasExit, state.attackerRegisterValues,
                                    state.attackerRegisterSinkTables);

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
