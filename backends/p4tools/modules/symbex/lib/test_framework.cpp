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

void TestFramework::writeTestToFile(const TamperingTestSpec *spec, cstring selectedBranches,
                                    size_t testIdx, float currentCoverage) {
    // Default: write each phase as a separate test file using the single-spec overload.
    writeTestToFile(spec->spec1, selectedBranches, testIdx * 3 - 2, currentCoverage, nullptr, 0);
    writeTestToFile(spec->spec2, selectedBranches, testIdx * 3 - 1, currentCoverage, nullptr, 0);
    writeTestToFile(spec->spec3, selectedBranches, testIdx * 3, currentCoverage, nullptr, 0);
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
