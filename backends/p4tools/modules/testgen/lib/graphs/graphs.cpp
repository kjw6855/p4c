/*
Copyright 2013-present Barefoot Networks, Inc.

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

#include "backends/p4tools/modules/testgen/lib/graphs/graphs.h"

#include "lib/crash.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/gc.h"
#include "lib/log.h"
#include "lib/nullstream.h"

namespace P4Tools::P4Testgen {

void Graphs::calc_ball_larus() {
    typedef std::vector<vertex_t> container;
    container c;
    boost::topological_sort(*g, std::back_inserter(c));
    for (container::iterator ii=c.begin(); ii!=c.end(); ++ii) {
        auto v = (*g)[*ii];
        Graphs::OutEdgeIterator eit, eend;
        std::tie(eit, eend) = boost::out_edges(*ii, *g);
        if (eit == eend) {
            // (1) leaf node
            boost::put(&Graphs::Vertex::numPaths, *g, *ii, 1);
        } else {
            // (2) node with edges
            big_int numPaths = 0;
            int numVertices = 0;
            for (; eit != eend; eit++) {
                auto e = *eit;
                auto u = boost::target(e, *g);
                boost::put(boost::edge_weight, g->root(), e, numPaths);
                numPaths += boost::get(&Graphs::Vertex::numPaths, *g, u);
                numVertices ++;
            }

            boost::put(&Graphs::Vertex::numPaths, *g, *ii, numPaths);
        }
    }
}

Graphs::vertex_t Graphs::add_vertex(const cstring &name, const IR::Node *node, VertexType type) {
    auto v = boost::add_vertex(*g);
    boost::put(&Vertex::name, *g, v, name);
    if (node != nullptr)
        boost::put(&Vertex::node, *g, v, node);
    boost::put(&Vertex::type, *g, v, type);
    return g->local_to_global(v);
}

void Graphs::add_edge(const vertex_t &from, const vertex_t &to, const cstring &name) {
    auto ep = boost::add_edge(from, to, g->root());
    boost::put(boost::edge_name, g->root(), ep.first, name);
}

void Graphs::add_edge(const vertex_t &from, const vertex_t &to, const cstring &name,
                      unsigned cluster_id) {
    auto ep = boost::add_edge(from, to, g->root());
    boost::put(boost::edge_name, g->root(), ep.first, name);

    auto attrs = boost::get(boost::edge_attribute, g->root());

    attrs[ep.first]["ltail"] = "cluster" + Util::toString(cluster_id - 2);
    attrs[ep.first]["lhead"] = "cluster" + Util::toString(cluster_id - 1);
}

void Graphs::limitStringSize(std::stringstream &sstream, std::stringstream &helper_sstream) {
    if (helper_sstream.str().size() > 25) {
        sstream << helper_sstream.str().substr(0, 25) << "...";
    } else {
        sstream << helper_sstream.str();
    }
    helper_sstream.str("");
    helper_sstream.clear();
}

std::optional<Graphs::vertex_t> Graphs::merge_other_statements_into_vertex() {
    if (statementsStack.empty()) return std::nullopt;
    std::stringstream sstream;
    std::stringstream helper_sstream;  // to limit line width

    if (statementsStack.size() == 1) {
        statementsStack[0]->dbprint(helper_sstream);
        limitStringSize(sstream, helper_sstream);
    } else if (statementsStack.size() == 2) {
        statementsStack[0]->dbprint(helper_sstream);
        limitStringSize(sstream, helper_sstream);
        sstream << "\\n";
        statementsStack[1]->dbprint(helper_sstream);
        limitStringSize(sstream, helper_sstream);
    } else {
        statementsStack[0]->dbprint(helper_sstream);
        limitStringSize(sstream, helper_sstream);
        sstream << "\\n...\\n";
        statementsStack.back()->dbprint(helper_sstream);
        limitStringSize(sstream, helper_sstream);
    }
    auto v = add_vertex(cstring(sstream), statementsStack[0], VertexType::STATEMENTS);
    for (auto parent : parents) add_edge(parent.first, v, parent.second->label());
    parents = {{v, new EdgeUnconditional()}};
    statementsStack.clear();
    return v;
}

Graphs::vertex_t Graphs::add_and_connect_vertex(const cstring &name, const IR::Node *node, VertexType type) {
    merge_other_statements_into_vertex();
    auto v = add_vertex(name, node, type);
    for (auto parent : parents) add_edge(parent.first, v, parent.second->label());
    return v;
}

}  // namespace P4Tools::P4Testgen
