/*
Copyright 2023-present Open Networking Foundation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <stdio.h>

#include <iostream>
#include <string>
#include <fstream>

#include "frontends/common/applyOptionsPragmas.h"
#include "frontends/common/parseInput.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "frontends/p4/frontend.h"

#include "ir/ir.h"
#include "ir/json_loader.h"
#include "ir/pass_utils.h"
#include "ir/pass_manager.h"
#include "lib/crash.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/gc.h"
#include "lib/log.h"
#include "lib/nullstream.h"
#include "metricsOptions.h"
#include "metricsPassManager.h"

namespace P4::P4Metrics {

class MidEnd : public P4::PassManager {
 public:
    P4::ReferenceMap refMap;
    P4::TypeMap typeMap;

    explicit MidEnd(MetricsOptions &options);
    void process(const IR::P4Program *&program) {
        program->apply(*this);
    }
};

MidEnd::MidEnd(MetricsOptions &options) {
    bool isv1 = options.langVersion == CompilerOptions::FrontendVersion::P4_14;
    refMap.setIsV1(isv1);
    P4Metrics::MetricsPassManager metricsPassManager(options, &refMap, &typeMap, options.customMetrics);
    setName("MidEnd");

    addPasses({
        new P4::TypeChecking(&refMap, &typeMap, true),  // update types
        new P4::EvaluatorPass(&refMap, &typeMap),
    });

    metricsPassManager.addInlined(*this);
    metricsPassManager.addUnusedCode(*this, false);
    metricsPassManager.addMetricPasses(*this);
}
}   // namespace P4::P4Metrics

using namespace P4;
using P4MetricsContext = P4CContextWithOptions<::P4Metrics::MetricsOptions>;

void compile(P4Metrics::MetricsOptions &options) {
    auto hook = options.getDebugHook();
    bool isv1 = options.langVersion == CompilerOptions::FrontendVersion::P4_14;
    if (isv1) {
        ::P4::error(ErrorType::ERR_UNSUPPORTED_ON_TARGET, "This compiler only handles P4-16");
        return;
    }
    const IR::P4Program *program = nullptr;

    if (options.loadIRFromJson) {
        std::filebuf fb;
        if (fb.open(options.file, std::ios::in) == nullptr) {
            ::P4::error(ErrorType::ERR_IO, "%s: No such file or directory.", options.file);
            return;
        }

        std::istream inJson(&fb);
        JSONLoader jsonFileLoader(inJson);
        if (!jsonFileLoader) {
            ::P4::error(ErrorType::ERR_IO, "%s: Not valid input file", options.file);
            return;
        }
        program = new IR::P4Program(jsonFileLoader);
        fb.close();
    } else {
        program = P4::parseP4File(options);
        if (::P4::errorCount() > 0) return;

        P4::P4COptionPragmaParser optionsPragmaParser(true);
        program->apply(P4::ApplyOptionsPragmas(optionsPragmaParser));

        P4::FrontEnd frontend;
        frontend.addDebugHook(hook);
        program = frontend.run(options, program);
        if (::P4::errorCount() > 0) return;
    }

    P4Metrics::MidEnd midEnd(options);
    midEnd.addDebugHook(hook);
    midEnd.process(program);
}

int main(int argc, char *const argv[]) {
    setup_gc_logging();
    setup_signals();

    AutoCompileContext autoMetricsContext(new P4MetricsContext);
    auto &options = P4MetricsContext::get().options();
    if (options.process(argc, argv) != nullptr) {
        if (options.loadIRFromJson == false) options.setInputFile();
    }
    if (::P4::errorCount() > 0) exit(1);

    try {
        compile(options);
    } catch (const std::exception &bug) {
        std::cerr << bug.what() << std::endl;
        return 1;
    }

    if (Log::verbose()) std::cerr << "Done." << std::endl;
    return ::P4::errorCount() > 0;
}