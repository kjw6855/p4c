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

#include "graphs.h"

#include "lib/crash.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/gc.h"
#include "lib/log.h"
#include "lib/nullstream.h"

namespace P4::graphs {

std::optional<Graphs::vertex_t> Graphs::find_node_by_name(Graph *g, const cstring &name) {
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        const auto &vinfo = (*g)[*vit];
        if (vinfo.name == name)
            return *vit;
    }

    return {};
}

std::optional<Graphs::vertex_t> Graphs::find_node_by_ptr(Graph *g, const IR::Node *ptr) {
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        const auto &vinfo = (*g)[*vit];
        for (auto node : vinfo.nodes)
            if (node == ptr)
                return *vit;
    }

    return {};
}

Graphs::vertex_t Graphs::add_vertex(const cstring &name, VertexType type, bool isStateful, const IR::Node *node) {
    auto v = boost::add_vertex(*g);
    boost::put(&Vertex::name, *g, v, name);
    boost::put(&Vertex::type, *g, v, type);
    boost::put(&Vertex::isStateful, *g, v, isStateful);
    if (node != nullptr) {
        auto &g_ref = *g;
        auto &v_nodes = g_ref[v].nodes;
        v_nodes.push_back(node);
    }
    return g->local_to_global(v);
}

Graphs::vertex_t Graphs::add_vertex_nodes(Graph *g, const cstring &name, VertexType type, bool isStateful, std::vector<const IR::Node *> &nodes) {
    auto v = boost::add_vertex(*g);
    boost::put(&Vertex::name, *g, v, name);
    boost::put(&Vertex::type, *g, v, type);
    boost::put(&Vertex::isStateful, *g, v, isStateful);

    auto &g_ref = *g;
    auto &v_nodes = g_ref[v].nodes;
    v_nodes.insert(v_nodes.end(), nodes.begin(), nodes.end());
    return g->local_to_global(v);
}

void Graphs::add_edge(Graph *g, const vertex_t &from, const vertex_t &to, const cstring &name, EdgeType type) {
    auto ep = boost::add_edge(from, to, g->root());
    boost::put(&Edge::name, g->root(), ep.first, name);
    boost::put(&Edge::type, g->root(), ep.first, type);
}

void Graphs::add_edge(const vertex_t &from, const vertex_t &to, const cstring &name, EdgeType type) {
    auto ep = boost::add_edge(from, to, g->root());
    boost::put(&Edge::name, g->root(), ep.first, name);
    boost::put(&Edge::type, g->root(), ep.first, type);
}

void Graphs::add_edge(const vertex_t &from, const vertex_t &to, const cstring &name,
                      EdgeType type, unsigned cluster_id) {
    auto ep = boost::add_edge(from, to, g->root());
    boost::put(&Edge::name, g->root(), ep.first, name);
    boost::put(&Edge::type, g->root(), ep.first, type);

    auto attrs = boost::get(boost::edge_attribute, g->root());

    attrs[ep.first]["ltail"_cs] = "cluster"_cs + Util::toString(cluster_id - 2);
    attrs[ep.first]["lhead"_cs] = "cluster"_cs + Util::toString(cluster_id - 1);
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

cstring Graphs::get_vertex_name(std::vector<const IR::Node *> nodes) {
    std::stringstream sstream;
    std::stringstream helper_sstream;  // to limit line width

    if (nodes.size() == 1) {
        nodes[0]->dbprint(helper_sstream);
        limitStringSize(sstream, helper_sstream);
    } else if (nodes.size() == 2) {
        nodes[0]->dbprint(helper_sstream);
        limitStringSize(sstream, helper_sstream);
        sstream << "\\n";
        nodes[1]->dbprint(helper_sstream);
        limitStringSize(sstream, helper_sstream);
    } else {
        nodes[0]->dbprint(helper_sstream);
        limitStringSize(sstream, helper_sstream);
        sstream << "\\n...\\n";
        nodes.back()->dbprint(helper_sstream);
        limitStringSize(sstream, helper_sstream);
    }

    return cstring(sstream);
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
    std::vector<const IR::Node*> nodes(statementsStack.begin(), statementsStack.end());
    auto v = add_vertex_nodes(g, cstring(sstream), VertexType::STATEMENTS, false, nodes);
    for (auto parent : parents) add_edge(parent.first, v, parent.second->label(), EdgeType::CONTROL);
    parents = {{v, new EdgeUnconditional()}};
    statementsStack.clear();
    return v;
}

Graphs::vertex_t Graphs::add_and_connect_vertex(const cstring &name, VertexType type,
                                                bool isStateful, const IR::Node *node) {
    merge_other_statements_into_vertex();
    auto v = add_vertex(name, type, isStateful, node);
    for (auto parent : parents) add_edge(parent.first, v, parent.second->label(), EdgeType::CONTROL);
    return v;
}

}  // namespace P4::graphs
