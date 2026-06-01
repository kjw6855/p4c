#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_OPTIONS_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_OPTIONS_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>

#include "lib/big_int.h"

#include "backends/p4tools/common/options.h"
#include "lib/cstring.h"
#include "midend/coverage.h"

#include "backends/p4tools/modules/symbex/core/symbolic_executor/path_selection.h"

namespace P4::P4Tools::Symbex {

/// Encapsulates and processes command-line options for Symbex.
class SymbexOptions : public AbstractP4cToolOptions {
 public:
    SymbexOptions();
    virtual ~SymbexOptions() = default;

    /// Maximum number of tests to be generated. Defaults to 1.
    int64_t maxTests = 1;

    /// Selects the path selection policy for test generation
    Symbex::PathSelectionPolicy pathSelectionPolicy = Symbex::PathSelectionPolicy::DepthFirst;

    /// List of the supported stop metrics.
    static const std::set<cstring> SUPPORTED_STOP_METRICS;

    // Stops generating tests when a particular metric is satisfied. Currently supported options are
    // listed in @var SUPPORTED_STOP_METRICS.
    cstring stopMetric;

    /// @returns the singleton instance of this class.
    static SymbexOptions &get();

    /// Directory for generated tests. Defaults to PWD.
    std::optional<std::filesystem::path> outputDir = std::nullopt;

    /// Fail on unimplemented features instead of trying the next branch
    bool strict = false;

    /// The test back end that Symbex will generate test for. Examples are STF, PTF or Protobuf.
    cstring testBackend;

    /// String of selected branches separated by comma.
    std::string selectedBranches;

    /// String of a pattern for resulting tests.
    std::string pattern;

    /// Track the branches that are executed in the symbolic executor. This can be used for
    /// deterministic replay of an execution trace.
    bool trackBranches = false;

    /// Build a DCG for input program. This control flow graph directed cyclic graph can be used
    /// for statement reachability analysis.
    bool dcg = false;

    /// The maximum permitted packet size, in bits.
    // The default is the standard MTU, 1500 bytes.
    int maxPktSize = 12000;

    /// The minimum permitted packet size, in bits.
    int minPktSize = 0;

    /// The list of permitted port ranges.
    /// TestGen will consider these when choosing input and output ports.
    std::vector<std::pair<int, int>> permittedPortRanges;

    /// Skip generating a control plane entry for the entities in this list.
    std::set<cstring> skippedControlPlaneEntities;

    /// Enforces the test generation of tests with mandatory output packet.
    bool outputPacketOnly = false;

    /// Enforces the test generation of tests with mandatory dropped packet.
    bool droppedPacketOnly = false;

    /// Require that the input port differ from the output port in generated terminal states.
    bool distinctIOPorts = false;

    bool interactive = true;

    /// Add conditions defined in assert/assume to the path conditions.
    /// Only tests which satisfy these conditions can be generated. This is active by default.
    bool enforceAssumptions = true;

    /// Produce only tests that violate the condition defined in assert calls.
    /// This will either produce no tests or only tests that contain counter examples.
    bool assertionModeEnabled = false;

    /// Specifies general options which IR nodes to track for coverage in the targeted P4 program.
    /// Multiple options are possible. Currently supported: STATEMENTS, TABLE_ENTRIES.
    P4::Coverage::CoverageOptions coverageOptions;

    int maxPortNo = 0;

    int grpcPort = 50051;

    bool measurePath = false;

    std::vector<int> allowPorts;

    /// Indicates that coverage tracking is enabled for some coverage criteria. This is used for
    /// sanity checking and it also affects information printed to output.
    bool hasCoverageTracking = false;

    /// Specifies minimum coverage that needs to be achieved for Symbex to exit successfully.
    float minCoverage = 0;

    /// The base name of the tests which are generated.
    /// Defaults to the name of the input program, if provided.
    std::optional<cstring> testBaseName;

    /// Indicates whether to build a dataflow dependency graph by using state_dependency module.
    bool stateDep = false;

    /// Attacker-chosen register value for the tampering scenario (--state-tamper-value).
    /// If set, Phase 2 writes this exact value to the register instead of a random one.
    /// Accepts decimal or 0x-prefixed hex (e.g. --state-tamper-value 0xdeadbeef).
    std::optional<big_int> stateTamperValue = std::nullopt;

    /// When true, RegisterAction.execute() always creates a symbolic TofinoRegisterValue
    /// test object regardless of the active test backend, and also emits a
    /// tofino_register_writeback continuation after the apply body so that
    /// withAttackerValues() can extract the written register value for Phase 2 tampering.
    /// Set by the tampering symbolic executor during both Phase 1 and Phase 2.
    bool tamperingRegisterTracking = false;

    /// When true, register initial values are zero-initialized instead of using free
    /// symbolic variables. Set only during Phase 1 (read phase) of tampering analysis so
    /// that Z3 explores paths with register value = 0 (hardware initial state), not arbitrary
    /// symbolic values. Must be false during Phase 2 so the write path remains reachable.
    bool initRegZeroValue = false;

 protected:
    bool validateOptions() const override;
};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_OPTIONS_H_ */
