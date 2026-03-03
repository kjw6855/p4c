/**
 * Copied from backends/graphs/controls.h
 */

#ifndef BACKENDS_STATE_DEPENDENCY_CONTROLS_H_
#define BACKENDS_STATE_DEPENDENCY_CONTROLS_H_

#include <optional>

#include "frontends/common/resolveReferences/resolveReferences.h"
#include "graphs.h"
#include "ir/ir.h"
#include "lib/hvec_map.h"

namespace P4::P4StateDependency {

class ControlGraphs : public Graphs,
                      public Inspector,
                      public P4WriteContext,
                      public P4::ResolutionContext {
 public:
    class ControlStack {
     public:
        Graph *pushBack(Graph &currentSubgraph, const cstring &name);
        Graph *popBack();
        Graph *getSubgraph() const;
        cstring getName(const cstring &name) const;
        bool isEmpty() const;

     private:
        std::vector<cstring> names{};
        std::vector<Graph *> subgraphs{};
    };

    ControlGraphs(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            std::filesystem::path graphsDir, bool setActionAsProc);

    enum { SKIPPING, NORMAL, READ_ONLY, WRITE_ONLY } state = SKIPPING;

    bool preorder(const IR::PackageBlock *block) override;
    bool preorder(const IR::ControlBlock *block) override;
    bool preorder(const IR::P4Control *cont) override;
    bool preorder(const IR::BlockStatement *statement) override;
    bool preorder(const IR::IfStatement *statement) override;
    bool preorder(const IR::SwitchStatement *statement) override;
    bool preorder(const IR::MethodCallStatement *statement) override;
    bool preorder(const IR::MethodCallExpression *mc) override;
    bool preorder(const IR::BaseAssignmentStatement *statement) override;
    bool preorder(const IR::Function *fn) override;
    bool preorder(const IR::ReturnStatement *) override;
    bool preorder(const IR::ExitStatement *) override;
    bool preorder(const IR::P4Table *table) override;
    bool preorder(const IR::Key *key) override;
    bool preorder(const IR::KeyElement *ke) override;
    bool preorder(const IR::P4Action *action) override;
    bool preorder(const IR::PathExpression *pe) override;

    bool isWrite(bool root_value = false);

    void visit_stateful(const cstring &name, const IR::Node *node,
                        const IR::Node *idx, bool isWrite,
                        const IR::Node *data=nullptr);
    void visit_call(const cstring &name, const IR::Node *node);

    const IR::Expression *add_variables(const IR::Expression *e, const Context *ctxt, bool isUsed);

    std::vector<Graph *> controlGraphsArray{};

    typedef std::pair<Graphs::vertex_t, Graphs::vertex_t> procedure_pair_t;
    static const procedure_pair_t emptyProcedure;

    procedure_pair_t getProcedure(const IR::Node *node) const {
        auto it = procedureGraphs.find(node);
        return it == procedureGraphs.end() ? emptyProcedure : it->second;
    }

 private:
    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;
    const cstring graphsDir;
    Parents return_parents{};
    bool setActionAsProc;

    ControlStack controlStack{};
    std::optional<cstring> instanceName{};
    hvec_map<const IR::Node *, procedure_pair_t> procedureGraphs;
    std::optional<Graphs::vertex_t> cur_v{};
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_CONTROLS_H_ */
