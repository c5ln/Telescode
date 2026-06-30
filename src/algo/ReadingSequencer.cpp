#include "ReadingSequencer.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <stdexcept>
#include <vector>

std::vector<NodeId> ReadingSequencer::sequence(const Graph&               g,
                                                 const std::vector<double>& combined_score,
                                                 const std::vector<int>&    loc_hint)
{
    const int N = g.size();
    if (N == 0) return {};
    if (combined_score.size() < static_cast<std::size_t>(N))
        throw std::invalid_argument("ReadingSequencer: combined_score.size() < g.size()");

    // Step 1: find SCCs and build condensation
    auto sccs = SCCFinder::find(g);

    // Map each node -> its SCC index
    std::vector<int> scc_of(N, -1);
    for (int i = 0; i < static_cast<int>(sccs.size()); ++i)
        for (NodeId u : sccs[i])
            scc_of[u] = i;

    const int S = static_cast<int>(sccs.size());

    // Condensation: compute super-node scores (max of members) and in-degrees
    std::vector<double> super_score(S, 0.0);
    for (int i = 0; i < S; ++i)
        for (NodeId u : sccs[i])
            super_score[i] = std::max(super_score[i], combined_score[u]);

    // Build condensation adjacency and in-degree.
    // Edge direction in adj: A→B means A imports/calls B (A depends on B).
    // We want to READ B before A, so the condensation must flow B→A (reversed),
    // making B's SCC have 0 in-degree and be processed first.
    std::vector<std::vector<int>> cond_adj(S);
    std::vector<int> indegree(S, 0);
    for (int u = 0; u < N; ++u) {
        for (NodeId v : g.adj[u]) {
            if (scc_of[u] != scc_of[v]) {
                // Reversed: v's SCC → u's SCC so importee SCC is processed first
                cond_adj[scc_of[v]].push_back(scc_of[u]);
            }
        }
    }
    // Deduplicate condensation edges and compute in-degrees
    for (int i = 0; i < S; ++i) {
        auto& edges = cond_adj[i];
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        for (int j : edges) ++indegree[j];
    }

    // Step 2: Kahn's on condensation with max-heap on super_score
    // Tie-break: score desc → (within SCC, handled per-node below)
    auto cmp_super = [&](int a, int b) {
        return super_score[a] < super_score[b];  // max-heap
    };
    std::priority_queue<int, std::vector<int>, decltype(cmp_super)> pq(cmp_super);

    for (int i = 0; i < S; ++i)
        if (indegree[i] == 0) pq.push(i);

    std::vector<NodeId> result;
    result.reserve(N);

    while (!pq.empty()) {
        int si = pq.top(); pq.pop();

        // Expand SCC: sort members by combined_score desc,
        // then loc_hint desc, then node name asc
        std::vector<NodeId> members = sccs[si];
        std::sort(members.begin(), members.end(), [&](NodeId a, NodeId b) {
            if (combined_score[a] != combined_score[b])
                return combined_score[a] > combined_score[b];
            const int la = a < static_cast<NodeId>(loc_hint.size()) ? loc_hint[a] : 0;
            const int lb = b < static_cast<NodeId>(loc_hint.size()) ? loc_hint[b] : 0;
            if (la != lb) return la > lb;
            const std::string& na = a < static_cast<NodeId>(g.node_to_id.size()) ? g.node_to_id[a] : "";
            const std::string& nb = b < static_cast<NodeId>(g.node_to_id.size()) ? g.node_to_id[b] : "";
            return na < nb;
        });

        for (NodeId u : members)
            result.push_back(u);

        for (int sj : cond_adj[si]) {
            if (--indegree[sj] == 0) pq.push(sj);
        }
    }

    return result;
}
