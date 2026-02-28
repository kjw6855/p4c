#ifndef BACKENDS_STATE_DEPENDENCY_NON_EXACT_TO_STATEFUL_H_
#define BACKENDS_STATE_DEPENDENCY_NON_EXACT_TO_STATEFUL_H_

#include "graphs.h"
#include "supergraphs.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

using Graph = Graphs::Graph;

class FindNonExactToStateful : public Graphs,
                               public Inspector {

 public:
    explicit FindNonExactToStateful(std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp> *graphProps)
        : controlGraphsArray(controlGraphsArray),
          graphProps(graphProps) {}
    Visitor::profile_t init_apply(const IR::Node *) override;

 private:
    void analyze_control_graph(Graph *g, SuperGraphProp &sgProp);

 protected:
    std::vector<Graph *> *controlGraphsArray{};
    std::vector<SuperGraphProp> *graphProps{};
};

class NonExactToStateful : public PassManager {
 public:
    explicit NonExactToStateful(std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp> *graphProps) {
        passes.push_back(new FindNonExactToStateful(controlGraphsArray, graphProps));
    }
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_NON_EXACT_TO_STATEFUL_H_ */
