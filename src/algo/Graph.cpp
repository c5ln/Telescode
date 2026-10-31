#include "Graph.h"


#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stack>
#include <unordered_set>
#include <utility>

NodeId Graph::get_or_add(const std::string& id)
{
    auto it = id_to_node.find(id);
    if (it != id_to_node.end()) return it->second;

    NodeId nid = static_cast<NodeId>(node_to_id.size());
    id_to_node[id] = nid;
    node_to_id.push_back(id);
    adj.emplace_back();
    radj.emplace_back();
    adj_w.emplace_back();
    return nid;
}

void Graph::add_edge(NodeId u, NodeId v, double w)
{
    if (u == v) return;
    if (u >= adj.size() || v >= radj.size()) return;
    if (adj_w.size() < adj.size()) adj_w.resize(adj.size());

    const uint64_t key = (static_cast<uint64_t>(u) << 32) | v;
    auto it = edge_pos_.find(key);
    if (it != edge_pos_.end()) {
        adj_w[u][it->second] += w;
        return;
    }

    edge_pos_[key] = static_cast<uint32_t>(adj[u].size());
    adj[u].push_back(v);
    adj_w[u].push_back(w);
    radj[v].push_back(u);
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

// ── GraphBuilder ────────────────────────────────────────────────────────────

static void add_edge(Graph& g, const std::string& src, const std::string& tgt, double w = 1.0)
{
    if (src == tgt) return;
    NodeId u = g.get_or_add(src);
    NodeId v = g.get_or_add(tgt);
    g.add_edge(u, v, w);
}

// Per-link-type base weight. INHERITS is the strongest coupling (you cannot
// understand a subclass without its base), CALLS next, IMPORTS weakest --
// a module-level import says far less than an actual call.
static double link_type_weight(const char* type, const AlgoConfig& cfg)
{
    if (!type) return cfg.edge_w_imports;
    if (std::strcmp(type, "INHERITS") == 0) return cfg.edge_w_inherits;
    if (std::strcmp(type, "CALLS")    == 0) return cfg.edge_w_calls;
    return cfg.edge_w_imports;
}

// `link` has one row per (source_id, target_id, link_type), so multiplicity only
// appears once entity ids are collapsed to file ids: `cnt` is the number of
// distinct entity pairs connecting the two files. Linear scaling lets one hot
// pair (hundreds of call sites) dominate the whole graph, so log is the default.
static double count_factor(int cnt, const AlgoConfig& cfg)
{
    if (cnt <= 1) return 1.0;
    switch (cfg.edge_count_mode) {
        case 0:  return 1.0;                                  // ignore multiplicity
        case 1:  return static_cast<double>(cnt);             // linear
        default: return 1.0 + std::log(static_cast<double>(cnt));  // log
    }
}

// Pass 1: file-level graph
// CALLS/INHERITS edges: both endpoints are file paths with '::' suffix stripped.
// IMPORTS edges: file id -> file id, as the resolver found them; an import of
// anything that is not a project file adds no edge.
// All project files are pre-populated as nodes so isolated files appear in the graph.
// Edge weight = link_type_weight * count_factor(#entity pairs), summed over types.
void GraphBuilder::build_file_graph(sqlite3* db, Graph& g, const AlgoConfig& cfg)
{
    // Load all project file_ids for node pre-population.
    std::unordered_set<std::string> file_ids;
    sqlite3_stmt* file_stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT file_id FROM file;", -1, &file_stmt, nullptr);
    if (file_stmt) {
        while (sqlite3_step(file_stmt) == SQLITE_ROW) {
            const char* fid = reinterpret_cast<const char*>(sqlite3_column_text(file_stmt, 0));
            if (fid) {
                file_ids.insert(fid);
                g.get_or_add(fid);
            }
        }
        sqlite3_finalize(file_stmt);
    }

    // CALLS and INHERITS: both source and target use file-path-based IDs.
    // GROUP BY (not DISTINCT) so `cnt` survives as edge weight.
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT"
        "    substr(source_id, 1, instr(source_id||'::', '::')-1) AS src,"
        "    substr(target_id, 1, instr(target_id||'::', '::')-1) AS tgt,"
        "    link_type, COUNT(*) AS cnt"
        " FROM link"
        " WHERE link_type IN ('CALLS', 'INHERITS')"
        " GROUP BY src, tgt, link_type;",
        -1, &stmt, nullptr);
    if (stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* src  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* tgt  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            const int   cnt  = sqlite3_column_int(stmt, 3);
            if (!src || !tgt) continue;
            if (file_ids.count(src) && file_ids.count(tgt))
                add_edge(g, src, tgt, link_type_weight(type, cfg) * count_factor(cnt, cfg));
        }
        sqlite3_finalize(stmt);
    }

    // IMPORTS: file id -> file id, one unit of weight per pair however many
    // specs name it. Read from import_resolution, the view's own source,
    // because the order matters and only it has the specs: add_edge appends
    // each new edge to the adjacency lists, Brandes and PageRank sum in that
    // order, and the link table this view replaced added a file's imports in
    // module-spec order. Ordering each pair by its first spec keeps that
    // order, and every score bit for bit; ordering by target id instead flips
    // betweenness ties at the last bit. After CALLS/INHERITS, as before.
    stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT file_id, resolved_file_id FROM import_resolution"
        " WHERE status = 'resolved'"
        " GROUP BY file_id, resolved_file_id"
        " ORDER BY file_id, MIN(module_spec);",
        -1, &stmt, nullptr);
    if (stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* src = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* tgt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (!src || !tgt) continue;
            if (file_ids.count(src) && file_ids.count(tgt))
                add_edge(g, src, tgt, cfg.edge_w_imports);
        }
        sqlite3_finalize(stmt);
    }
}

// Pass 2: function/class-level graph
// Nodes: function_id, class_id. Edges: CALLS + INHERITS only.
// No multiplicity here -- `link` has one row per (source_id, target_id, link_type) --
// so the weight is purely the link-type weight.
// Every function and class is pre-populated as a node, as files are in pass 1:
// an entity with no resolved link (only unresolved calls, or none) would
// otherwise be missing from the reading sequence altogether. Ordered by id so
// node numbering, and with it every tie-break, does not depend on row order.
void GraphBuilder::build_func_graph(sqlite3* db, Graph& g, const AlgoConfig& cfg)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT function_id FROM function"
        " UNION SELECT class_id FROM class"
        " ORDER BY 1;",
        -1, &stmt, nullptr);
    if (stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (id) g.get_or_add(id);
        }
        sqlite3_finalize(stmt);
    }

    // The view's CALLS / INHERITS rows, read off ref so that they can be
    // ordered by ref_id: edges go in in that order, which the scores depend
    // on (see IMPORTS above). idx_ref_link has them in exactly this order.
    stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT source_id, resolved_id, kind FROM ref"
        " WHERE emits_link = 1 AND kind IN ('CALLS', 'INHERITS')"
        " ORDER BY kind, ref_id;",
        -1, &stmt, nullptr);
    if (!stmt) return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* src  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* tgt  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (!src || !tgt) continue;

        NodeId u = g.get_or_add(src);
        NodeId v = g.get_or_add(tgt);
        g.add_edge(u, v, link_type_weight(type, cfg));
    }
    sqlite3_finalize(stmt);
}

// Build entity_id -> file_id map for all functions (and classes via function's class_id).
// Also populates entity_type_map and entity_start_line.
void GraphBuilder::build_entity_file_map(sqlite3* db,
                                          std::unordered_map<std::string, std::string>& m)
{
    // Functions: each function belongs to a file directly, or via its class.
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT f.function_id, COALESCE(f.file_id, c.file_id) AS file_id"
        " FROM function f"
        " LEFT JOIN class c ON f.class_id = c.class_id;",
        -1, &stmt, nullptr);
    if (stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* fid  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* file = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (fid && file) m[fid] = file;
        }
        sqlite3_finalize(stmt);
    }

    // Classes: each class belongs to a file directly.
    stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT class_id, file_id FROM class;",
        -1, &stmt, nullptr);
    if (stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* cid  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* file = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (cid && file) m[cid] = file;
        }
        sqlite3_finalize(stmt);
    }
}

static void build_extra_maps(sqlite3* db,
                              std::unordered_map<std::string, std::string>& type_map,
                              std::unordered_map<std::string, int>&         start_line_map,
                              std::unordered_map<std::string, int>&         file_loc_map)
{
    sqlite3_stmt* stmt = nullptr;

    // functions: type + start_line
    sqlite3_prepare_v2(db,
        "SELECT function_id, start_line FROM function;",
        -1, &stmt, nullptr);
    if (stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (id) {
                type_map[id]       = "function";
                start_line_map[id] = sqlite3_column_int(stmt, 1);
            }
        }
        sqlite3_finalize(stmt);
    }

    // classes: type + start_line
    stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT class_id, start_line FROM class;",
        -1, &stmt, nullptr);
    if (stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (id) {
                type_map[id]       = "class";
                start_line_map[id] = sqlite3_column_int(stmt, 1);
            }
        }
        sqlite3_finalize(stmt);
    }

    // files: loc
    stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT file_id, raw_loc FROM file;",
        -1, &stmt, nullptr);
    if (stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (id) file_loc_map[id] = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }
}

GraphBuilderResult GraphBuilder::build(sqlite3* db, const AlgoConfig& cfg)
{
    GraphBuilderResult result;
    build_file_graph(db, result.file_graph, cfg);
    build_func_graph(db, result.func_graph, cfg);
    build_entity_file_map(db, result.entity_file_map);
    build_extra_maps(db, result.entity_type_map,
                         result.entity_start_line,
                         result.file_loc_map);
    return result;
}
