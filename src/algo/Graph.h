#pragma once

#include "AlgoConfig.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>

struct sqlite3;

using NodeId = uint32_t;

struct Graph {
    std::vector<std::vector<NodeId>> adj;
    std::vector<std::vector<NodeId>> radj;
    // adj_w[u][i] is the weight of the edge adj[u][i]. Maintained only by
    // add_edge(); graphs assembled by pushing into adj/radj directly leave it
    // empty and are treated as unweighted (see edge_weight()).
    std::vector<std::vector<double>> adj_w;
    std::unordered_map<std::string, NodeId> id_to_node;
    std::vector<std::string>                node_to_id;

    NodeId get_or_add(const std::string& id);

    // Inserts u→v, or adds `w` to the existing edge's weight. Keeps adj/radj
    // free of parallel edges so SCC and Brandes see a simple graph; multiplicity
    // lives in the weight, which only PageRank consumes.
    void add_edge(NodeId u, NodeId v, double w = 1.0);

    // Weight of adj[u][i]. Falls back to 1.0 when adj_w is not populated for u,
    // so a hand-built (test) graph behaves exactly as it did before weighting.
    double edge_weight(NodeId u, std::size_t i) const
    {
        if (u >= adj_w.size() || adj_w[u].size() != adj[u].size()) return 1.0;
        return adj_w[u][i];
    }

    // Σ of outgoing edge weights; equals out-degree for an unweighted graph.
    double out_weight(NodeId u) const
    {
        if (u >= adj.size()) return 0.0;
        double s = 0.0;
        for (std::size_t i = 0; i < adj[u].size(); ++i) s += edge_weight(u, i);
        return s;
    }

    int size() const { return static_cast<int>(node_to_id.size()); }

private:
    // (u<<32 | v) -> index into adj[u]; build-time only, for add_edge dedup.
    std::unordered_map<uint64_t, uint32_t> edge_pos_;
};

// ── Python import resolution ────────────────────────────────────────────────
// Exposed (rather than file-local to Graph.cpp) because these two rules decide
// the entire IMPORTS edge set, and bench/features/extract.py has to reproduce
// them exactly -- if the two drift, in_deg/out_deg describe a different graph
// than the one PageRank ran on.

// Directory prefixes under which a top-level package may live, inferred from
// where __init__.py files sit: if `D/P/__init__.py` exists and `D/__init__.py`
// does not, then P is a top-level package and `D/` is a layout root.
// Always contains "" (repo root). Sorted shortest-first, so a root-layout repo
// resolves exactly as it did before this existed.
// max_depth caps how deep a root may be; test-fixture trees are full of
// package-looking directories and would otherwise become resolution candidates.
std::vector<std::string> inferPackageRoots(const std::unordered_set<std::string>& file_ids,
                                            int max_depth = 1);

// Resolve one IMPORTS target to a project file_id, or "" if it does not name a
// project file (an external module such as `os` or `numpy`).
//
// `module` is recorded verbatim by the parser, so it is either
//   absolute: "pkg.core.dataset"
//   relative: ".mod", "..pkg.mod", or bare dots (".", "..") for `from . import x`
// Relative targets resolve against `source_file`'s directory: one leading dot is
// the current package, each further dot goes up one more level. Going above the
// repo root is unresolvable, not an error.
std::string resolveModule(const std::string& module,
                          const std::string& source_file,
                          const std::unordered_set<std::string>& file_ids,
                          const std::vector<std::string>& roots);

class SCCFinder {
public:
    static std::vector<std::vector<NodeId>> find(const Graph& g);
};

// ── GraphBuilder ────────────────────────────────────────────────────────────
// DB에서 파일/함수 그래프와 엔티티 메타데이터 맵을 구축한다.

struct GraphBuilderResult {
    Graph file_graph;
    Graph func_graph;
    // entity_id -> file_id (for Pass 2 entities: functions and classes)
    std::unordered_map<std::string, std::string> entity_file_map;
    // entity_id -> "function" or "class"
    std::unordered_map<std::string, std::string> entity_type_map;
    // entity_id -> start_line (for local_rank tie-breaking)
    std::unordered_map<std::string, int>         entity_start_line;
    // file_id -> loc (for file-level pass loc_hint)
    std::unordered_map<std::string, int>         file_loc_map;
};

class GraphBuilder {
public:
    static GraphBuilderResult build(sqlite3* db, const AlgoConfig& cfg = {});

private:
    static void build_file_graph(sqlite3* db, Graph& g, const AlgoConfig& cfg);
    static void build_func_graph(sqlite3* db, Graph& g, const AlgoConfig& cfg);
    static void build_entity_file_map(sqlite3* db,
                                      std::unordered_map<std::string, std::string>& m);
};
