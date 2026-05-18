#include "Graph.h"

#include <algorithm>
#include <cstdio>
#include <stack>

NodeId Graph::get_or_add(const std::string& id)
{
    auto it = id_to_node.find(id);
    if (it != id_to_node.end()) return it->second;

    NodeId nid = static_cast<NodeId>(node_to_id.size());
    id_to_node[id] = nid;
    node_to_id.push_back(id);
    adj.emplace_back();
    radj.emplace_back();
    return nid;
}

// ── SCCFinder ─────────────────────────────────────────────────────────────────

static constexpr int UNVISITED = -1;

struct TarjanState {
    const Graph&                      g;
    std::vector<int>                  disc;
    std::vector<int>                  low;
    std::vector<bool>                 on_stack;
    std::stack<NodeId>                stk;
    std::vector<std::vector<NodeId>>  sccs;
    int                               timer = 0;

    explicit TarjanState(const Graph& g_)
        : g(g_)
        , disc(g_.size(), UNVISITED)
        , low(g_.size(), 0)
        , on_stack(g_.size(), false)
    {}

    void dfs(NodeId u)
    {
        disc[u] = low[u] = timer++;
        stk.push(u);
        on_stack[u] = true;

        for (NodeId v : g.adj[u]) {
            if (disc[v] == UNVISITED) {
                dfs(v);
                low[u] = std::min(low[u], low[v]);
            } else if (on_stack[v]) {
                low[u] = std::min(low[u], disc[v]);
            }
        }

        if (low[u] == disc[u]) {
            std::vector<NodeId> scc;
            while (true) {
                NodeId w = stk.top(); stk.pop();
                on_stack[w] = false;
                scc.push_back(w);
                if (w == u) break;
            }
            if (scc.size() > 10) {
                std::fprintf(stderr,
                    "SCCFinder: large SCC detected (size=%zu)\n", scc.size());
            }
            sccs.push_back(std::move(scc));
        }
    }
};

std::vector<std::vector<NodeId>> SCCFinder::find(const Graph& g)
{
    TarjanState state(g);
    for (int u = 0; u < g.size(); ++u)
        if (state.disc[u] == UNVISITED)
            state.dfs(static_cast<NodeId>(u));
    return std::move(state.sccs);
}
