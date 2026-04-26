#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_EXPR_VISITOR_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_EXPR_VISITOR_H_

#include <cstdint>
#include <string>

#include "ir/id.h"
#include "ir/ir.h"
#include "ir/vector.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/small_visit/expr_visitor.h"
#include "backends/p4tools/modules/symbex/core/small_visit/small_visit.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/concolic.h"

namespace P4::P4Tools::Symbex::Bmv2 {

class Bmv2V1ModelExprVisitor : public ExprVisitor {
 protected:
    std::string getClassName() override;

 private:
    /// In the behavioral model, checksum functions have the following signature.
    using ChecksumFunction = std::function<big_int(const char *buf, size_t len)>;

    /// Chunk size is 8 bits, i.e., a byte.
    static constexpr int CHUNK_SIZE = 8;

    // Helper function that checks whether the given structure filed has a 'field_list' annotation
    // and the recirculate index matches. @returns true if that is the case.
    static bool isPartOfFieldList(const IR::StructField *field, uint64_t recirculateIndex);

    /// This is a utility function for recirculation externs. This function resets all the values
    /// associated with @ref unless a value contained in the Type_StructLike type of ref has an
    /// annotation associated with it. If the annotation index matches @param recirculateIndex, the
    /// reference is not reset.
    void resetPreservingFieldList(ExecutionState &nextState, const IR::PathExpression *ref,
                                  uint64_t recirculateIndex) const;

    /// Helper function, which is triggered when clone was called in the P4 program.
    void processClone(const ExecutionState &state, SmallStepEvaluator::Result &result);

    /// Helper function, which is triggered when resubmit or recirculate was called in the P4
    /// program.
    void processRecirculate(const ExecutionState &state, SmallStepEvaluator::Result &result);

    /// Call into a behavioral model helper function to compute the appropriate checksum. The
    /// checksum is determined by @param algo.
    big_int computeChecksum(const std::vector<const IR::Expression *> &exprList,
                                      Bmv2HashAlgorithm algo);

    /// Converts a big integer input into a vector of bytes. This byte vector is fed into the
    /// hash function.
    /// This function mimics the conversion of data structures to bytes in the behavioral model.
    static std::vector<uint8_t> convertBigIntToBytes(big_int &dataInt, int targetWidthBits, bool padLeft);

 public:
    Bmv2V1ModelExprVisitor(ExecutionState &state,
                           const ProgramInfo &programInfo, TestCase &testCase);

    static const ExprVisitor::ExternMethodImpls<Bmv2V1ModelExprVisitor>::MethodImpl
        ASSERT_ASSUME_EXECUTE;
    static const ExprVisitor::ExternMethodImpls<Bmv2V1ModelExprVisitor> BMV2_EXTERN_METHOD_IMPLS;

    void evalExternMethodCall(const ExternInfo &externInfo) override;

    bool preorder(const IR::P4Table * /*table*/) override;
};
}  // namespace P4::P4Tools::Symbex::Bmv2

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_EXPR_VISITOR_H_ */
