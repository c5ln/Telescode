#include "GraphBuilder.h"

#include <sqlite3.h>
#include <unordered_set>

// Resolve a Python module name to a file_id if it exists in the file table.
// Tries: module.name -> module/name.py, then module/name/__init__.py
static std::string resolve_module(const std::string& module,
                                   const std::unordered_set<std::string>& file_ids)
{
    std::string path = module;
    for (char& c : path) if (c == '.') c = '/';

    std::string candidate = path + ".py";
    if (file_ids.count(candidate)) return candidate;

    candidate = path + "/__init__.py";
    if (file_ids.count(candidate)) return candidate;

    return {};
}

static void add_edge(Graph& g, const std::string& src, const std::string& tgt)
{
    if (src == tgt) return;
    NodeId u = g.get_or_add(src);
    NodeId v = g.get_or_add(tgt);
    g.adj[u].push_back(v);
    g.radj[v].push_back(u);
}

// Pass 1: file-level graph
// CALLS/INHERITS edges: both endpoints are file paths with '::' suffix stripped.
// IMPORTS edges: target is a Python module name; resolved to file path if possible.
// External module names that can't be resolved to a project file are excluded.
// All project files are pre-populated as nodes so isolated files appear in the graph.
void GraphBuilder::build_file_graph(sqlite3* db, Graph& g)
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
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT DISTINCT"
        "    substr(source_id, 1, instr(source_id||'::', '::')-1) AS src,"
        "    substr(target_id, 1, instr(target_id||'::', '::')-1) AS tgt"
        " FROM link"
        " WHERE link_type IN ('CALLS', 'INHERITS');",
        -1, &stmt, nullptr);
    if (stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* src = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* tgt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (!src || !tgt) continue;
            if (file_ids.count(src) && file_ids.count(tgt))
                add_edge(g, src, tgt);
        }
        sqlite3_finalize(stmt);
    }

    // IMPORTS: source is a file path; target is a Python module name.
    // Resolve module name to file path; skip unresolvable (external) modules.
    stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT DISTINCT"
        "    substr(source_id, 1, instr(source_id||'::', '::')-1) AS src,"
        "    target_id AS module_name"
        " FROM link"
        " WHERE link_type = 'IMPORTS';",
        -1, &stmt, nullptr);
    if (stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* src = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* mod = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (!src || !mod) continue;
            if (!file_ids.count(src)) continue;
            std::string tgt = resolve_module(mod, file_ids);
            if (!tgt.empty())
                add_edge(g, src, tgt);
        }
        sqlite3_finalize(stmt);
    }
}

// Pass 2: function/class-level graph
// Nodes: function_id, class_id. Edges: CALLS + INHERITS only.
void GraphBuilder::build_func_graph(sqlite3* db, Graph& g)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT source_id, target_id FROM link"
        " WHERE link_type IN ('CALLS', 'INHERITS');",
        -1, &stmt, nullptr);
    if (!stmt) return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* src = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* tgt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (!src || !tgt) continue;

        NodeId u = g.get_or_add(src);
        NodeId v = g.get_or_add(tgt);
        g.adj[u].push_back(v);
        g.radj[v].push_back(u);
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
        "SELECT file_id, loc FROM file;",
        -1, &stmt, nullptr);
    if (stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (id) file_loc_map[id] = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }
}

GraphBuilderResult GraphBuilder::build(sqlite3* db)
{
    GraphBuilderResult result;
    build_file_graph(db, result.file_graph);
    build_func_graph(db, result.func_graph);
    build_entity_file_map(db, result.entity_file_map);
    build_extra_maps(db, result.entity_type_map,
                         result.entity_start_line,
                         result.file_loc_map);
    return result;
}
