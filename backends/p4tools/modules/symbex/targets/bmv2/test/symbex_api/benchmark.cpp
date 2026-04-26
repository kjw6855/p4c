#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "backends/p4tools/common/compiler/context.h"
#include "backends/p4tools/common/lib/logging.h"
#include "frontends/common/options.h"
#include "lib/compile_context.h"
#include "test/gtest/helpers.h"

#include "backends/p4tools/modules/symbex/core/symbolic_executor/path_selection.h"
#include "backends/p4tools/modules/symbex/options.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/test/gtest_utils.h"
#include "backends/p4tools/modules/symbex/symbex.h"

namespace P4::P4Tools::Test {

using namespace P4::literals;

class SymbexBenchmark : public SymbexBmv2Test {};

TEST_F(SymbexBenchmark, SuccessfullyGenerate1000Tests) {
    auto &symbexOptions = Symbex::SymbexOptions::get();
    symbexOptions.target = "bmv2"_cs;
    symbexOptions.arch = "v1model"_cs;
    auto includePath = P4CTestEnvironment::getProjectRoot() / "p4include";
    symbexOptions.preprocessor_options = "-I" + includePath.string();
    auto fabricFile =
        P4CTestEnvironment::getProjectRoot() / "testdata/p4_16_samples/fabric_20190420/fabric.p4";
    symbexOptions.file = fabricFile.string();
    symbexOptions.testBackend = "PROTOBUF_IR"_cs;
    symbexOptions.testBaseName = "dummy"_cs;
    symbexOptions.seed = 1;
    // Fix the packet size.
    symbexOptions.minPktSize = 512;
    symbexOptions.maxPktSize = 512;
    // Select a random path for each test.
    symbexOptions.pathSelectionPolicy = P4::P4Tools::Symbex::PathSelectionPolicy::RandomBacktrack;
    // Generate 2000 tests.
    symbexOptions.maxTests = 2000;
    // This enables performance printing.
    P4Tools::enablePerformanceLogging();

    auto testList = Symbex::Symbex::generateTests(symbexOptions);
    ASSERT_TRUE(testList.has_value());

    // Print the report.
    P4Tools::printPerformanceReport();
}
}  // namespace P4::P4Tools::Test
