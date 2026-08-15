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

    /// Which tampering search phases use a shared (global) traversal instead of a per-chain one.
    /// Defaults to Phase1, which is the long-standing behaviour.
    Symbex::SharedTraversalMode sharedTraversal = Symbex::SharedTraversalMode::Phase1;

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

    /// Opt-in whole-pipeline state-dependency: analyze Parser->Ingress/Egress as one IFDS supergraph so
    /// cross-block chains (header->metadata provenance set in the parser) are visible. Only affects the
    /// in-process compute path; loading a cache built with --whole-pipeline yields cross-block chains
    /// regardless. Off by default.
    bool wholePipeline = false;

    /// Opt-in: process SOChains by security impact (sink reaches an enforcement primitive) instead
    /// of by chain id. Ordering only — never changes acceptance, emission, or the solver; it decides
    /// which chains a run cut short by a timeout gets to. Composes with any --shared-traversal value.
    bool chainImpactOrder = false;

    /// Opt-in parser-deps mode: compute the parser-state dependency record (header-derived metadata) and
    /// seed those metadata fields as per-control IFDS sources, so chains root at parser-derived metadata.
    /// Lightweight alternative to --whole-pipeline. Cache loads carry the per-chain header pins regardless.
    bool parserDeps = false;

    /// When set, load pre-computed SOChains from this JSON cache (produced by p4c_state_dependency
    /// --cache-chains) instead of running the IFDS analysis in-process. Hard-errors if the file is
    /// missing or its embedded source hash / arch does not match this program.
    std::optional<std::string> stateDepCachePath;

    /// When set, load external control-plane / port annotations from this JSON file. Supplies
    /// facts the P4 source cannot express: which principal roles may write a state object, and
    /// control-plane assumptions about table entries (p4v-style predicates, SIGCOMM'18). Absent
    /// => behaviour is exactly as before. Hard-errors if the file is missing or unparseable.
    std::optional<std::string> cpAnnotationPath;

    /// When set, write a per-sink control-plane report to this JSON path during tampering
    /// generation: each chain sink with its action list, const-entry status and key-space coverage,
    /// which actions are observably distinct from the default, and a pre-filled `assume` skeleton
    /// (both the action and the `action_data` form) ready to paste into a --cp-annotation file.
    /// Report-only: it never changes which tests are generated.
    std::optional<std::string> cpStubsPath;

    /// When set, run the in-process state-dependency analysis on this program's post-midend IR,
    /// serialize the resulting SOChains to this JSON path, and exit before symbolic execution. The
    /// cache is written from the exact IR a later --state-dep-cache load re-resolves against, so it
    /// aligns by construction (unlike a p4c_state_dependency --cache-chains cache, which is built on
    /// pre-midend IR). Used by the nightly cache builder.
    std::optional<std::string> dumpStateDepCachePath;

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

    /// When true, a register READ returns a FRESH symbolic variable even when a carried
    /// register IndexMap exists, instead of the carried folded constant. Used ONLY by the
    /// Phase-2 precondition-discovery DFS: a guard `reg[I] == C` becomes satisfiable against the
    /// relaxed read, and the relaxed terminal's model then yields the required precondition
    /// (regName, index I, value C). The emitted test never runs in this mode — it is a discovery
    /// oracle, not an execution semantics change.
    bool relaxCarriedRegisterRead = false;

    /// Safety guardrail bounding the length of a multi-packet Phase-2 sequence (setup packets +
    /// the write packet). The actual count is data-driven (the unrolling stops as soon as Z3 says
    /// the write is satisfiable, or at a fixpoint); this cap only guarantees termination when a
    /// precondition is never reachable. NOT a target depth. Set via --max-phase2-packets.
    int64_t maxPhase2Packets = 64;

    /// Bound on how many times a Phase-2 tampering candidate whose write cell does not match the
    /// cell Phase 1 read may be re-derived by steering the attacker packet's hash inputs to the
    /// victim's values. Budget is per Phase-1 condition bucket and one retry is spent per distinct
    /// hash call site, so a program writing the register from several sites needs several retries.
    /// 0 disables steering entirely, degrading "steer, else drop" to plain "drop" -- the A/B lever
    /// that separates how many cases steering rescues from how many were already on the right cell.
    /// Set via --max-index-steer-retries.
    int64_t maxIndexSteerRetries = 2;

    /// When true, a table whose default action Phase 1 overrode is pinned to that same action and
    /// action data in Phases 2 and 3. Without it the phases are independent Z3 queries that share
    /// action-argument symbol names by accident, so they may disagree on the control-plane
    /// configuration the emitted test is replayed under. Cleared by
    /// --no-cross-phase-default-action-pin, which restores the unpinned behaviour for A/B runs.
    bool crossPhaseDefaultActionPin = true;

    /// When true, the emission solve additionally asserts that each written register index really
    /// evaluates to the cell recorded in the test, so the packet, the ports and
    /// affected_register.index are consistent by construction. Programs that branch on hash bits
    /// have already committed to an arbitrary cell along the path, so the assert can be UNSAT;
    /// those cases fall back to reporting the index without enforcing it. Cleared by
    /// --no-pin-index-value, which forces every case onto that report-only fallback -- the A/B
    /// lever that counts how many cases the enforced pin actually holds for.
    bool pinIndexValue = true;

    /// When true, run an ADDITIONAL tampering pass that looks for a sink table moving from one of
    /// its `const entries` to another — a different action, or the same action under different
    /// compile-time action data. Restricted to const-entry tables on purpose: there the
    /// key -> (action, args) map comes from the P4 program, so a difference between two entries is a
    /// property of the program. On an open (control-plane) table p4symbex synthesises the entries
    /// itself, so both sides of such a comparison would be tool-chosen and the question degenerates;
    /// that is why the mode never applies there. Purely additive — the HIT->MISS and MISS->HIT
    /// passes are untouched, and with this off generation is byte-identical. Set via
    /// --const-entry-action-divergence.
    ///
    /// KNOWN LIMITATION: the search reaches the attack Phase-3 outcome through the ACCUMULATION
    /// driver, which replays the same Phase-2 packet. That finds a divergence when replaying changes
    /// the register (a counter, or a read-modify-write RegisterAction), but not when the attacker
    /// writes a value of its own choosing with a plain `write()`: replaying an idempotent write can
    /// never move the value off whatever the Phase-2 solver happened to pick, so the goal fails even
    /// though the emission path would have forced the packet to carry --state-tamper-value. Making
    /// that shape work needs the written VALUE steered toward a value selecting a different const
    /// entry (they are statically known) rather than accumulated -- the driveRegisterPhase2
    /// "pre-set the carried SO and re-validate the write path" pattern.
    bool constEntryActionDivergence = false;

 protected:
    bool validateOptions() const override;
};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_OPTIONS_H_ */
