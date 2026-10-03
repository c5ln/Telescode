#include "Graph.h"

#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stack>
#include <unordered_set>

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

namespace {

// Directory part of a file_id, without the trailing slash. "a.py" -> "".
std::string dir_of(const std::string& file_id)
{
    const std::size_t slash = file_id.find_last_of('/');
    return slash == std::string::npos ? std::string() : file_id.substr(0, slash);
}

// Strip `levels` trailing components. Returns false if that would go above the
// repo root -- `from ..x import y` in a top-level module has nothing to climb to.
bool climb(std::string& dir, int levels)
{
    for (int i = 0; i < levels; ++i) {
        if (dir.empty()) return false;
        const std::size_t slash = dir.find_last_of('/');
        dir = (slash == std::string::npos) ? std::string() : dir.substr(0, slash);
    }
    return true;
}

// "pkg/core" + "mod.sub" -> "pkg/core/mod/sub"; an empty tail keeps the base.
std::string join_module(const std::string& base, const std::string& dotted)
{
    std::string tail = dotted;
    for (char& c : tail) if (c == '.') c = '/';
    if (tail.empty()) return base;
    return base.empty() ? tail : base + "/" + tail;
}

// A module path resolves to either the module file or the package __init__.
std::string file_for(const std::string& path,
                     const std::unordered_set<std::string>& file_ids)
{
    if (path.empty()) return {};
    std::string candidate = path + ".py";
    if (file_ids.count(candidate)) return candidate;
    candidate = path + "/__init__.py";
    if (file_ids.count(candidate)) return candidate;
    return {};
}

}  // namespace

std::vector<std::string> inferPackageRoots(const std::unordered_set<std::string>& file_ids,
                                            int max_depth)
{
    static const std::string kInit = "/__init__.py";

    std::unordered_set<std::string> packages;
    for (const std::string& fid : file_ids) {
        if (fid.size() > kInit.size() &&
            fid.compare(fid.size() - kInit.size(), kInit.size(), kInit) == 0)
            packages.insert(fid.substr(0, fid.size() - kInit.size()));
    }

    std::unordered_set<std::string> roots{""};
    for (const std::string& pkg : packages) {
        const std::string parent = dir_of(pkg);
        if (parent.empty()) continue;              // already at the repo root
        if (packages.count(parent)) continue;      // a nested package, not top-level
        int depth = 1;
        for (char c : parent) if (c == '/') ++depth;
        if (depth > max_depth) continue;           // too deep to be a layout root
        roots.insert(parent + "/");
    }

    std::vector<std::string> out(roots.begin(), roots.end());
    // Shortest first so "" wins whenever the repo is root-layout; ties broken
    // lexicographically to keep the graph reproducible across runs.
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return out;
}

std::string resolveModule(const std::string& module,
                          const std::string& source_file,
                          const std::unordered_set<std::string>& file_ids,
                          const std::vector<std::string>& roots)
{
    if (module.empty()) return {};

    if (module[0] == '.') {
        // Relative: resolve against the importing file's own directory, so the
        // layout roots below play no part -- the anchor is already inside the tree.
        std::size_t dots = 0;
        while (dots < module.size() && module[dots] == '.') ++dots;

        std::string base = dir_of(source_file);
        // One dot is the current package (the directory the file sits in, which
        // for pkg/__init__.py is pkg itself); each extra dot climbs one level.
        if (!climb(base, static_cast<int>(dots) - 1)) return {};

        const std::string tail = module.substr(dots);
        if (tail.empty()) {
            // `from . import x` -- the target is the package itself.
            if (base.empty()) return {};
            const std::string init = base + "/__init__.py";
            return file_ids.count(init) ? init : std::string();
        }
        return file_for(join_module(base, tail), file_ids);
    }

    // Absolute: try each layout root, shortest first.
    for (const std::string& root : roots) {
        std::string hit = file_for(join_module(root.empty() ? std::string()
                                                            : root.substr(0, root.size() - 1),
                                               module),
                                    file_ids);
        if (!hit.empty()) return hit;
    }
    return {};
}

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

// `link` has PRIMARY KEY(source_id, target_id, link_type), so multiplicity only
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
// IMPORTS edges: target is a Python module name; resolved to file path if possible.
// External module names that can't be resolved to a project file are excluded.
// All project files are pre-populated as nodes so isolated files appear in the graph.
// Edge weight = link_type_weight * count_factor(#entity pairs), summed over types.
void GraphBuilder::build_file_graph(sqlite3* db, Graph& g, const AlgoConfig& cfg)
{
    // Load all project file_ids for node pre-population and module resolution.
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

    // IMPORTS: source is a file path; target is a Python module name, absolute
    // or relative. Relative targets need the source file, so it is passed in.
    // Unresolvable (external) modules are skipped.
    const std::vector<std::string> roots = inferPackageRoots(file_ids);
    stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT"
        "    substr(source_id, 1, instr(source_id||'::', '::')-1) AS src,"
        "    target_id AS module_name, COUNT(*) AS cnt"
        " FROM link"
        " WHERE link_type = 'IMPORTS'"
        " GROUP BY src, module_name;",
        -1, &stmt, nullptr);
    if (stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* src = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* mod = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const int   cnt = sqlite3_column_int(stmt, 2);
            if (!src || !mod) continue;
            if (!file_ids.count(src)) continue;
            std::string tgt = resolveModule(mod, src, file_ids, roots);
            if (!tgt.empty())
                add_edge(g, src, tgt, cfg.edge_w_imports * count_factor(cnt, cfg));
        }
        sqlite3_finalize(stmt);
    }
}

// Pass 2: function/class-level graph
// Nodes: function_id, class_id. Edges: CALLS + INHERITS only.
// No multiplicity here -- (source_id, target_id, link_type) is the `link` PK --
// so the weight is purely the link-type weight.
void GraphBuilder::build_func_graph(sqlite3* db, Graph& g, const AlgoConfig& cfg)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT source_id, target_id, link_type FROM link"
        " WHERE link_type IN ('CALLS', 'INHERITS');",
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
