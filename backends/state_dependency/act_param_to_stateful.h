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
    explicit FindActParamToStateful(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs)
        : refMap(refMap),
          typeMap(typeMap),
          controlGraphsArray(controlGraphsArray),
          graphProps(graphProps),
          genSupergraphs(genSupergraphs) {}
    Visitor::profile_t init_apply(const IR::Node *) override;

 private:
    void analyze_control_graph(Tabulation *tab);
    std::vector<const IR::Node *> get_var_members(Tabulation *tab, const IR::Node *var);

 protected:
    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;
    std::vector<Graph *> *controlGraphsArray{};
    std::vector<SuperGraphProp *> *graphProps{};
    GenSGMode genSupergraphs;
};

class ActParamToStateful : public PassManager {
 public:
    explicit ActParamToStateful(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs) {
        passes.push_back(new FindActParamToStateful(refMap, typeMap,
                    controlGraphsArray,
                    graphProps, genSupergraphs));
    }
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_ACT_PARAM_TO_STATEFUL_H_ */
