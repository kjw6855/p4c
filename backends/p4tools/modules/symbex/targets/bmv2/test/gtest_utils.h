#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_TEST_GTEST_UTILS_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_TEST_GTEST_UTILS_H_

#include "backends/p4tools/modules/symbex/test/gtest_utils.h"
#include "backends/p4tools/modules/symbex/test/small-step/util.h"

namespace P4::P4Tools::Test {

/// Sets up the correct context for a Symbex BMv2 test.
class SymbexBmv2Test : public SymbexTest {
    std::unique_ptr<AutoCompileContext> compileContext;

 public:
    void SetUp() override {
        compileContext = SymbexTest::SetUp("bmv2", "v1model");
        if (compileContext == nullptr) {
            FAIL() << "Failed to set up Symbex BMv2 test";
            return;
        }
    }
};

/// Creates a test case with the @hdrFields for stepping on an @expr.
std::optional<const P4ToolsTestCase> createBmv2V1modelSmallStepExprTest(
    const std::string &hdrFields, const std::string &expr);

class Bmv2SmallStepTest : public SmallStepTest {
    std::unique_ptr<AutoCompileContext> compileContext;

 public:
    void SetUp() override {
        compileContext = SymbexTest::SetUp("bmv2", "v1model");
        if (compileContext == nullptr) {
            FAIL() << "Failed to set up Symbex BMv2 test";
            return;
        }
    }
};

}  // namespace P4::P4Tools::Test

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_TEST_GTEST_UTILS_H_ */
