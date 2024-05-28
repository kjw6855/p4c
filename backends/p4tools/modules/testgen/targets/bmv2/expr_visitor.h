#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_BMV2_EXPR_VISITOR_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_BMV2_EXPR_VISITOR_H_

#include <cstdint>
#include <string>

#include "ir/id.h"
#include "ir/ir.h"
#include "ir/vector.h"

#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/core/small_visit/expr_visitor.h"
#include "backends/p4tools/modules/testgen/core/small_visit/small_visit.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"

namespace P4Tools::P4Testgen::Bmv2 {

class Bmv2V1ModelExprVisitor : public ExprVisitor {
 protected:
    std::string getClassName() override;

 private:
    /// In the behavioral model, checksum functions have the following signature.
    using ChecksumFunction = std::function<big_int(const char *buf, size_t len)>;

    /// Chunk size is 8 bits, i.e., a byte.
    static constexpr int CHUNK_SIZE = 8;

    /// We are not using an enum class because we directly compare integers. This is because error
    /// types are converted into integers in our interpreter. If we use an enum class, we have to
    /// cast every enum access to int.
    struct Bmv2HashAlgorithm {
        using Type = enum {
            crc32,
            crc32_custom,
            crc16,
            crc16_custom,
            random,
            identity,
            csum16,
            xor16
        };
    };

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
                                      int algo);

    /// Converts a big integer input into a vector of bytes. This byte vector is fed into the
    /// hash function.
    /// This function mimics the conversion of data structures to bytes in the behavioral model.
    static std::vector<char> convertBigIntToBytes(big_int &dataInt, int targetWidthBits);

 public:
    Bmv2V1ModelExprVisitor(ExecutionState &state,
                           const ProgramInfo &programInfo, TestCase &testCase);

    void evalExternMethodCall(const IR::MethodCallExpression *call, const IR::Expression *receiver,
                              IR::ID name, const IR::Vector<IR::Argument> *args,
                              ExecutionState &state) override;

    bool preorder(const IR::P4Table * /*table*/) override;
};
}  // namespace P4Tools::P4Testgen::Bmv2

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_BMV2_EXPR_VISITOR_H_ */
