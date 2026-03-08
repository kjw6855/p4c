#ifndef BACKENDS_STATE_DEPENDENCY_NON_EXACT_TO_STATEFUL_H_
#define BACKENDS_STATE_DEPENDENCY_NON_EXACT_TO_STATEFUL_H_

#include "graphs.h"
#include "supergraphs.h"
#include "tabulation.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

using Graph = Graphs::Graph;

class FindNonExactToStateful : public Graphs,
                               public Inspector {

 public:
    explicit FindNonExactToStateful(std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs)
        : controlGraphsArray(controlGraphsArray),
          graphProps(graphProps),
          genSupergraphs(genSupergraphs) {}
    Visitor::profile_t init_apply(const IR::Node *) override;

 private:
    void analyze_control_graph(Tabulation *tab);
    void collect_non_exact_fields(Tabulation *tab,
            hvec_map<Graphs::vertex_t, std::vector<const IR::Node *>> &fields);

 protected:
    std::vector<Graph *> *controlGraphsArray{};
    std::vector<SuperGraphProp *> *graphProps{};
    GenSGMode genSupergraphs;
};

class NonExactToStateful : public PassManager {
 public:
    explicit NonExactToStateful(std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs) {
        passes.push_back(new FindNonExactToStateful(controlGraphsArray,
                    graphProps, genSupergraphs));
    }
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_NON_EXACT_TO_STATEFUL_H_ */
