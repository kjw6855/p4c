/*
 * Copyright (c) 2017 VMware Inc. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_LIB_GRAPHS_PARSERS_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_LIB_GRAPHS_PARSERS_H_

#include "frontends/common/resolveReferences/referenceMap.h"
#include "backends/p4tools/modules/testgen/lib/graphs/graphs.h"
#include "ir/ir.h"
#include "lib/cstring.h"
#include "lib/nullstream.h"
#include "lib/path.h"
#include "lib/safe_vector.h"

namespace P4Tools::P4Testgen {

class ParserGraphs : public Graphs {
 protected:
    struct TransitionEdge {
        const IR::ParserState *sourceState;
        const IR::ParserState *destState;
        cstring label;

        TransitionEdge(const IR::ParserState *source, const IR::ParserState *dest, cstring label)
            : sourceState(source), destState(dest), label(label) {}
    };

    std::map<const IR::P4Parser *, safe_vector<const TransitionEdge *>> transitions;
    std::map<const IR::P4Parser *, safe_vector<const IR::ParserState *>> states;

 public:
    ParserGraphs(P4::ReferenceMap *refMap);

    void calc_ball_larus_on_graphs();
    Graph *CreateSubGraph(Graph &currentSubgraph, const cstring &name);
    void postorder(const IR::P4Parser *parser) override;
    void postorder(const IR::ParserState *state) override;
    void postorder(const IR::PathExpression *expression) override;
    void postorder(const IR::SelectExpression *expression) override;

    std::vector<Graph *> parserGraphsArray{};

 private:
    P4::ReferenceMap *refMap;
    std::optional<cstring> instanceName{};
};

}  // namespace P4Tools::P4Testgen

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_LIB_GRAPHS_PARSERS_H_ */
