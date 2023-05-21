#ifndef BACKENDS_P4TOOLS_MODULES_FUZZER_TARGETS_BMV2_EXPR_VISITOR_H_
#define BACKENDS_P4TOOLS_MODULES_FUZZER_TARGETS_BMV2_EXPR_VISITOR_H_

#include <stdint.h>

#include <string>

#include "ir/id.h"
#include "ir/ir.h"
#include "ir/vector.h"

#include "backends/p4tools/modules/fuzzer/core/program_info.h"
#include "backends/p4tools/modules/fuzzer/core/small_step/visit_stepper.h"
#include "backends/p4tools/modules/fuzzer/lib/visit_state.h"

namespace P4Tools {

namespace P4Testgen {

namespace Bmv2 {

class BMv2_V1ModelVisitStepper : public VisitStepper {
 protected:
    std::string getClassName() override;

 private:
    // Helper function that checks whether the given structure filed has a 'field_list' annotation
    // and the recirculate index matches. @returns true if that is the case.
    static bool isPartOfFieldList(const IR::StructField* field, uint64_t recirculateIndex);

    /// This is a utility function for recirculation externs. This function resets all the values
    /// associated with @ref unless a value contained in the Type_StructLike type of ref has an
    /// annotation associated with it. If the annotation index matches @param recirculateIndex, the
    /// reference is not reset.
    void resetPreservingFieldList(VisitState* nextState, const IR::PathExpression* ref,
                                  uint64_t recirculateIndex) const;

 public:
    BMv2_V1ModelVisitStepper(VisitState& state, const ProgramInfo& programInfo, const TestCase& testCase);

    void evalExternMethodCall(const IR::MethodCallExpression* call, const IR::Expression* receiver,
                              IR::ID name, const IR::Vector<IR::Argument>* args,
                              VisitState& state) override;

    bool preorder(const IR::P4Table* /*table*/) override;
};
}  // namespace Bmv2

}  // namespace P4Testgen

}  // namespace P4Tools

#endif /* BACKENDS_P4TOOLS_MODULES_FUZZER_TARGETS_BMV2_EXPR_VISITOR_H_ */
