// src/ui/class_diagram/cd_builder.cpp

#include "cd_builder.h"
#include "../ts_style.h"
#include <sqlite3.h>
#include <unordered_map>
#include <cmath>
#include <cstring>
#include <string>

namespace TS {
namespace {

// "src/web/controller.py" → "web.controller"
std::string packageFromFileId(const std::string& fid)
{
    std::string p = fid;
    for (const char* prefix : {"src/", "lib/", "app/"}) {
        if (p.rfind(prefix, 0) == 0) { p = p.substr(std::strlen(prefix)); break; }
    }
    auto dot = p.rfind('.');
    if (dot != std::string::npos) p = p.substr(0, dot);
    for (char& c : p) if (c == '/') c = '.';
    return p;
}

// Python naming convention → UML access modifier
char accessFromName(const std::string& name)
{
    if (name.size() >= 4 &&
        name[0] == '_' && name[1] == '_' &&
        name[name.size()-2] == '_' && name[name.size()-1] == '_')
        return '+';  // dunder: __init__ etc. are public API
    if (name.size() >= 2 && name[0] == '_' && name[1] == '_')
        return '-';  // name-mangled private
    if (name[0] == '_')
        return '#';  // convention: protected
    return '+';
}

void AssignGridPositions(CDGraph& graph)
{
    const int n = static_cast<int>(graph.nodes.size());
    if (n == 0) return;
    const int cols      = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n)))));
    const float col_step = (NODE_WIDTH + 80.0f);
    const float row_step = 220.0f;
    for (int i = 0; i < n; ++i) {
        graph.nodes[i].pos = {
            static_cast<float>(i % cols) * col_step,
            static_cast<float>(i / cols) * row_step
        };
    }
}

} // anonymous namespace

CDGraph BuildCDGraph(sqlite3* db)
{
    CDGraph graph;
    if (!db) return graph;

    sqlite3_stmt* stmt = nullptr;

    // ── Step 1: classes ────────────────────────────────────────────────────
    std::unordered_map<std::string, int>    classToNodeId;
    std::unordered_map<std::string, size_t> classToIdx;

    sqlite3_prepare_v2(db,
        "SELECT class_id, file_id, class_name FROM class ORDER BY file_id, class_name;",
        -1, &stmt, nullptr);

    int node_id = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CDNode node;
        node.node_id    = node_id;
        node.class_id   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        node.class_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        node.package    = packageFromFileId(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        classToNodeId[node.class_id] = node_id;
        classToIdx[node.class_id]    = static_cast<size_t>(node_id);
        graph.nodes.push_back(std::move(node));
        ++node_id;
    }
    sqlite3_finalize(stmt);

    // ── Step 2: params (skip self/cls, preserve ordinal order) ────────────
    std::unordered_map<std::string, std::string> funcParams;

    sqlite3_prepare_v2(db,
        "SELECT p.function_id, p.param_name"
        "  FROM param p"
        "  JOIN function f ON f.function_id = p.function_id"
        " WHERE f.class_id IS NOT NULL AND f.nesting_depth = 1"
        "   AND p.param_name NOT IN ('self','cls')"
        " ORDER BY p.function_id, p.ordinal;",
        -1, &stmt, nullptr);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string fid   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string pname = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        auto it = funcParams.find(fid);
        if (it == funcParams.end()) funcParams[fid] = pname;
        else                        it->second += ", " + pname;
    }
    sqlite3_finalize(stmt);

    // ── Step 3: methods → attach to nodes ─────────────────────────────────
    sqlite3_prepare_v2(db,
        "SELECT function_id, class_id, function_name"
        "  FROM function"
        " WHERE class_id IS NOT NULL AND nesting_depth = 1"
        " ORDER BY class_id, start_line;",
        -1, &stmt, nullptr);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string fid   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string cid   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        std::string fname = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        auto it = classToIdx.find(cid);
        if (it == classToIdx.end()) continue;

        CDMethod m;
        m.access   = accessFromName(fname);
        m.name     = fname;
        m.params   = funcParams.count(fid) ? funcParams[fid] : "";
        m.ret_type = "void";  // return types not stored in DB
        graph.nodes[it->second].methods.push_back(std::move(m));
    }
    sqlite3_finalize(stmt);

    // ── Step 4: Fields → attach to nodes ──────────────────────────────────────
    sqlite3_prepare_v2(db,
        "SELECT class_id, field_name, access FROM field ORDER BY class_id, field_name;",
        -1, &stmt, nullptr);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string cid   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string fname = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* acc   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        auto it = classToIdx.find(cid);
        if (it == classToIdx.end()) continue;

        CDField f;
        f.access = acc ? acc[0] : '+';
        f.name   = fname;
        graph.nodes[it->second].fields.push_back(std::move(f));
    }
    sqlite3_finalize(stmt);

    // ── Step 5: Association edges — CALLS (function→function, class→class) ─
    int edge_id = 0;

    sqlite3_prepare_v2(db,
        "SELECT DISTINCT sf.class_id, tf.class_id"
        "  FROM link l"
        "  JOIN function sf ON sf.function_id = l.source_id"
        "  JOIN function tf ON tf.function_id = l.target_id"
        " WHERE l.link_type = 'CALLS'"
        "   AND sf.class_id IS NOT NULL"
        "   AND tf.class_id IS NOT NULL"
        "   AND sf.class_id != tf.class_id;",
        -1, &stmt, nullptr);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string src = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string dst = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        auto si = classToNodeId.find(src);
        auto di = classToNodeId.find(dst);
        if (si == classToNodeId.end() || di == classToNodeId.end()) continue;
        graph.edges.push_back({edge_id++, si->second, di->second, CDEdgeType::Association});
    }
    sqlite3_finalize(stmt);

    // ── Step 6: Dependency edges — INHERITS (class→base_class_name) ────────
    sqlite3_prepare_v2(db,
        "SELECT DISTINCT l.source_id, c2.class_id"
        "  FROM link l"
        "  JOIN class c2 ON c2.class_name = l.target_id"
        " WHERE l.link_type = 'INHERITS'"
        "   AND l.source_id != c2.class_id;",
        -1, &stmt, nullptr);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string src = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string dst = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        auto si = classToNodeId.find(src);
        auto di = classToNodeId.find(dst);
        if (si == classToNodeId.end() || di == classToNodeId.end()) continue;
        graph.edges.push_back({edge_id++, si->second, di->second, CDEdgeType::Dependency});
    }
    sqlite3_finalize(stmt);

    // ── Step 7: initial grid layout ────────────────────────────────────────
    AssignGridPositions(graph);

    return graph;
}

} // namespace TS
