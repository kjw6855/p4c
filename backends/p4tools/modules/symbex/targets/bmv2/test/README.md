<!-- 
Documentation Inclusion:
This README is integrated as a subsection of the "Symbex" page in the P4 compiler documentation.

Refer to the specific section here: [Symbex BMv2 target tests - Subsection](https://p4lang.github.io/p4c/symbex.html#symbex-bmv2-target-tests)
-->
# Symbex BMv2 target tests

## CMake Files

+ P4Tests.cmake - Common test suite to add P4 tests from P4C submodules.
  + Run symbex on P4-16 V1Model p4s with the BMv2 target.
+ BMV2...Xfail.cmake - BMv2 xfails for the various BMv2 V1Model back ends.

## How to Run tests

+ All P4C submodule tests are tagged with 'symbex-p4c-bmv2' label

```bash
cd build/symbex
ctest -R symbex-p4c-bmv2
```

