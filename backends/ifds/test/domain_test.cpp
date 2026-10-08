#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <set>

#include "backends/ifds/controls.h"
#include "backends/ifds/domain.h"
#include "backends/ifds/ide_pass.h"
#include "backends/ifds/supergraphs.h"
#include "backends/ifds/tabulation.h"
#include "backends/ifds/utils.h"
#include "frontends/common/resolveReferences/referenceMap.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "frontends/p4/typeChecking/typeChecker.h"
#include "frontends/p4/typeMap.h"
#include "test/gtest/helpers.h"

namespace P4::IFDS::Test {

using namespace P4::literals;

namespace {

/// A domain whose seeds come from a callback.
class FnDomain : public IDEDomain {
 public:
    FnDomain(cstring n, std::function<std::vector<TabVertex>(Tabulation *)> f)
        : n(n), f(std::move(f)) {}
    cstring name() const override { return n; }
    std::vector<TabVertex> seeds(Tabulation *tab) override { return f(tab); }

 private:
    cstring n;
    std::function<std::vector<TabVertex>(Tabulation *)> f;
};

/// Exposes the multi-domain solve of IDEPass.
class SolvePass : public IDEPass {
 public:
    using IDEPass::IDEPass;
    using IDEPass::solve;
    Visitor::profile_t init_apply(const IR::Node *n) override { return Inspector::init_apply(n); }

 protected:
    void analyze_control_graph(Tabulation *) override {}
    std::vector<TabVertex> get_seeds(Tabulation *) override { return {}; }
};

using FactKey = std::pair<Graphs::vertex_t, const IR::Node *>;
using Projection = std::map<FactKey, std::set<FactKey>>;

/// Seeds of domain d reaching each fact, after solving @domains on graph @i.
Projection solveAndProject(SolvePass &pass, Graphs::Graph *g, SuperGraphProp *sgProp,
                           DomainSet &domains, size_t d) {
    Tabulation tab(g, sgProp);
    Projection out;
    if (!pass.solve(&tab, domains)) return out;
    for (const auto &[tv, value] : tab.valueMap) {
        std::set<FactKey> seeds;
        for (const auto &s : domains.targets(tab, value, d)) seeds.insert({s.node, s.var});
        if (!seeds.empty()) out[{tv.node, tv.var}] = seeds;
    }
    return out;
}

const char *kProgram = R"(
struct meta_t { bit<32> a; bit<32> b; }
struct headers_t { }
parser P(packet_in pkt, out headers_t hdr, inout meta_t m, inout standard_metadata_t sm) {
    state start { transition accept; }
}
control V(inout headers_t hdr, inout meta_t m) { apply { } }
control I(inout headers_t hdr, inout meta_t m, inout standard_metadata_t sm) {
    register<bit<32>, bit<32>>(16) r;
    action store(bit<32> v) { r.write(0, v); }
    action fwd(bit<9> p) { sm.egress_spec = p; }
    table t1 { key = { m.a: exact; } actions = { store; } }
    table t2 { key = { m.b: exact; } actions = { fwd; } }
    apply {
        t1.apply();
        bit<32> x;
        r.read(x, 0);
        m.b = x;
        t2.apply();
    }
}
control E(inout headers_t hdr, inout meta_t m, inout standard_metadata_t sm) { apply { } }
control C(inout headers_t hdr, inout meta_t m) { apply { } }
control D(packet_out pkt, in headers_t hdr) { apply { } }
V1Switch(P(), V(), I(), E(), C(), D()) main;
)";

}  // namespace

class IfdsDomainTest : public P4::Test::P4CTest {};

TEST_F(IfdsDomainTest, CombinedSolveEqualsSeparateSolves) {
    auto test = P4::Test::FrontendTestCase::create(P4_SOURCE(P4Headers::V1MODEL, kProgram));
    ASSERT_TRUE(test);

    P4::ReferenceMap refMap;
    P4::TypeMap typeMap;
    IR::ToplevelBlock *toplevel = nullptr;
    auto *evaluator = new P4::EvaluatorPass(&refMap, &typeMap);
    PassManager prep({new P4::TypeChecking(&refMap, &typeMap, true), evaluator,
                      [&toplevel, evaluator]() { toplevel = evaluator->getToplevelBlock(); }});
    const auto *program = test->program->apply(prep);
    ASSERT_TRUE(program && toplevel);

    ControlGraphs cgen(&refMap, &typeMap, {}, "v1model"_cs);
    cgen.genSupergraphs = GenSGMode::FULL;
    toplevel->getMain()->apply(cgen);
    SuperGraphs sg(&refMap, &typeMap, &cgen.controlGraphsArray, &cgen.graphVars,
                   &cgen.graphLocalVars, &cgen.procOfs, &cgen.callMaps, &cgen.procCallerMaps,
                   &cgen.retArgEdges, &cgen.actionMaps, &cgen.headerVarNames,
                   &cgen.ingressPortVars, &cgen.egressPortVars, &cgen.dropVars);
    sg.gen_supergraphs();
    SolvePass pass(&refMap, &typeMap, &cgen.controlGraphsArray, &sg.graphProps, GenSGMode::FULL);

    FnDomain act("Act"_cs, [](Tabulation *tab) { return tab->sgProp->actionParams; });
    FnDomain actCopy("Act2"_cs, [](Tabulation *tab) { return tab->sgProp->actionParams; });
    FnDomain so("SO"_cs, [&refMap, &typeMap](Tabulation *tab) {
        return collect_state_vars(tab->g, tab->sgProp, &refMap, &typeMap);
    });

    size_t checkedGraphs = 0;
    for (size_t i = 0; i < cgen.controlGraphsArray.size(); i++) {
        auto *g = cgen.controlGraphsArray[i];
        auto *sgProp = sg.graphProps[i];
        if (sgProp->actionParams.empty()) continue;
        checkedGraphs++;

        DomainSet actOnly({&act});
        DomainSet soOnly({&so});
        DomainSet both({&act, &so});
        DomainSet twice({&act, &actCopy});
        auto sepAct = solveAndProject(pass, g, sgProp, actOnly, 0);
        auto sepSO = solveAndProject(pass, g, sgProp, soOnly, 0);
        EXPECT_FALSE(sepAct.empty()) << "action params reach no fact; the test is vacuous";
        EXPECT_FALSE(sepSO.empty()) << "register reads reach no fact; the test is vacuous";

        // Each domain's slice of a combined solve equals the domain solved alone.
        EXPECT_EQ(solveAndProject(pass, g, sgProp, both, 0), sepAct);
        EXPECT_EQ(solveAndProject(pass, g, sgProp, both, 1), sepSO);
        // Seeds shared by two domains (same facts) generate both domains' bits.
        EXPECT_EQ(solveAndProject(pass, g, sgProp, twice, 0), sepAct);
        EXPECT_EQ(solveAndProject(pass, g, sgProp, twice, 1), sepAct);
    }
    EXPECT_GT(checkedGraphs, 0u);
}

}  // namespace P4::IFDS::Test
