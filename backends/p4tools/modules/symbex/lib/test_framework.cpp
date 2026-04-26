#include "backends/p4tools/modules/symbex/lib/test_framework.h"

#include "backends/p4tools/modules/symbex/lib/exceptions.h"

namespace P4::P4Tools::Symbex {

TestFramework::TestFramework(const TestBackendConfiguration &testBackendConfiguration)
    : testBackendConfiguration(testBackendConfiguration) {}

const TestBackendConfiguration &TestFramework::getTestBackendConfiguration() const {
    return testBackendConfiguration.get();
}

bool TestFramework::isInFileMode() const {
    return getTestBackendConfiguration().fileBasePath.has_value();
}

AbstractTestReferenceOrError TestFramework::produceTest(const TestSpec * /*spec*/,
                                                        cstring /*selectedBranches*/,
                                                        size_t /*testIdx*/,
                                                        float /*currentCoverage*/,
                                                        unsigned char* /*testCoverage*/,
                                                        int /*mapSize*/) {
    SYMBEX_UNIMPLEMENTED("produceTest() not implemented for this test framework.");
}

}  // namespace P4::P4Tools::Symbex
