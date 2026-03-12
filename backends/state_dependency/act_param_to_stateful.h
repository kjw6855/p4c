#ifndef BACKENDS_STATE_DEPENDENCY_ACT_PARAM_TO_STATEFUL_H_
#define BACKENDS_STATE_DEPENDENCY_ACT_PARAM_TO_STATEFUL_H_

#include "graphs.h"
#include "supergraphs.h"
#include "tabulation.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

using Graph = Graphs::Graph;

class FindActParamToStateful : public Graphs,
                               public Inspector {

 public:
    explicit FindActParamToStateful(std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs)
        : controlGraphsArray(controlGraphsArray),
          graphProps(graphProps),
          genSupergraphs(genSupergraphs) {}
    Visitor::profile_t init_apply(const IR::Node *) override;

 private:
    void analyze_control_graph(Tabulation *tab);

 protected:
    std::vector<Graph *> *controlGraphsArray{};
    std::vector<SuperGraphProp *> *graphProps{};
    GenSGMode genSupergraphs;
};

class ActParamToStateful : public PassManager {
 public:
    explicit ActParamToStateful(std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs) {
        passes.push_back(new FindActParamToStateful(controlGraphsArray,
                    graphProps, genSupergraphs));
    }
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_ACT_PARAM_TO_STATEFUL_H_ */
