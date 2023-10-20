#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_LIB_TABLE_COLLECTOR_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_LIB_TABLE_COLLECTOR_H_

#include <set>
#include <vector>
#include <map>

#include "ir/ir.h"
#include "ir/irutils.h"
#include "ir/visitor.h"
#include "lib/source_file.h"
#include "midend/coverage.h"

#include "backends/p4tools/common/lib/symbolic_env.h"
#include "backends/p4tools/common/lib/table_utils.h"
#include "backends/p4tools/common/core/abstract_execution_state.h"
#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/p4testgen.pb.h"
#include "backends/p4tools/modules/testgen/lib/continuation.h"

using p4testgen::TestCase;

namespace P4Tools::P4Testgen {

class TableExecutionState : public AbstractExecutionState {
 public:
    ~TableExecutionState() override = default;

    /// @see Taint::hasTaint
    bool hasTaint(const IR::Expression *expr) const;

    [[nodiscard]] TableExecutionState &clone() const override;

    [[nodiscard]] const IR::Expression *get(const IR::StateVariable &var) const override;

    void set(const IR::StateVariable &var, const IR::Expression *value) override;

    explicit TableExecutionState(const IR::P4Program *program);

};

class TableCollector : public Inspector {
    Continuation::Body body;
    Continuation::Body tmpBody;
    std::set<const IR::P4Table*> p4Tables;
    std::map<cstring, P4::Coverage::CoverageSet> actionMap;
    P4::Coverage::CoverageSet actionNodes;
    bool enableDump = false;

    //bool preorder(const IR::Node *node) override;
    bool preorder(const IR::P4Control *p4control) override;
    bool preorder(const IR::MethodCallStatement *methodCallStatement) override;
    bool preorder(const IR::P4Table *p4table) override;
    bool preorder(const IR::P4Action *p4action) override;

 public:
    const IR::P4Control *parentControl = nullptr;
    explicit TableCollector();

    void findP4Actions();
    const Continuation::Body &getP4Tables() const;
    const std::set<const IR::P4Table*> &getP4TableSet() const;
    const P4::Coverage::CoverageSet *getActions(cstring tableName) const;
    const P4::Coverage::CoverageSet &getActionNodes() const;
};

}  // namespace P4Tools::P4Testgen

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_LIB_TABLE_COLLECTOR_H_ */
