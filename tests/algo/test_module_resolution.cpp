#include "algo/Graph.h"
#include "db/db.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <string>
#include <vector>

// Module resolution itself is tested in tests/lang/test_python_language.cpp,
// and through the resolver in tests/resolve/test_resolver.cpp.

// ── Through GraphBuilder ──────────────────────────────────────────────────────
// An IMPORTS target is the file the resolver found, so the graph takes it as
// it is: no resolveModule, and every spelling of one import is the same single
// link.

namespace {

void exec_sql(sqlite3* db, const char* sql)
{
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "");
    sqlite3_free(err);
}

double edge_weight(const Graph& g, const std::string& src, const std::string& tgt)
{
    const NodeId u = g.id_to_node.at(src);
    const NodeId v = g.id_to_node.at(tgt);
    for (std::size_t i = 0; i < g.adj[u].size(); ++i)
        if (g.adj[u][i] == v) return g.edge_weight(u, i);
    return -1.0;
}

}  // namespace

TEST(GraphBuilderImports, TargetsAreFileIds)
{
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);

    exec_sql(db,
        "INSERT INTO file(file_id, file_name, language, raw_loc) VALUES"
        " ('pkg/__init__.py','__init__.py','python',10),"
        " ('pkg/core.py','core.py','python',10),"
        " ('pkg/util.py','util.py','python',10),"
        " ('app.py','app.py','python',10);");
    exec_sql(db,
        "INSERT INTO import_resolution(file_id, module_spec, resolved_file_id, status) VALUES"
        // Two spellings of one import: one link.
        " ('pkg/core.py','.util','pkg/util.py','resolved'),"
        " ('pkg/core.py','pkg.util','pkg/util.py','resolved'),"
        " ('app.py','pkg.core','pkg/core.py','resolved'),"
        // A package importing itself: a link, but no edge.
        " ('pkg/__init__.py','.','pkg/__init__.py','resolved'),"
        // Neither names a project file.
        " ('app.py','os',NULL,'external'),"
        " ('app.py','pkg.gone',NULL,'unresolved');");

    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT source_id || ' -> ' || target_id FROM link WHERE link_type = 'IMPORTS'"
        " ORDER BY 1;", -1, &stmt, nullptr), SQLITE_OK);
    std::vector<std::string> links;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        links.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    EXPECT_EQ(links, (std::vector<std::string>{
        "app.py -> pkg/core.py",
        "pkg/__init__.py -> pkg/__init__.py",
        "pkg/core.py -> pkg/util.py",
    }));

    AlgoConfig cfg;
    cfg.edge_count_mode = 1;   // linear: a second row would double the weight
    auto gbr = GraphBuilder::build(db, cfg);
    const Graph& g = gbr.file_graph;

    EXPECT_NEAR(edge_weight(g, "pkg/core.py", "pkg/util.py"), cfg.edge_w_imports, 1e-12);
    EXPECT_NEAR(edge_weight(g, "app.py", "pkg/core.py"), cfg.edge_w_imports, 1e-12);
    EXPECT_TRUE(g.adj[g.id_to_node.at("pkg/__init__.py")].empty());
    EXPECT_EQ(g.size(), 4);

    sqlite3_close(db);
}

TEST(GraphBuilderImports, ImportOfAFileNotScannedAddsNothing)
{
    // resolved_file_id has no foreign key; an id with no file row (a database
    // edited by hand, say) must not turn into a node.
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);
    exec_sql(db,
        "INSERT INTO file(file_id, file_name, language, raw_loc) VALUES ('a.py','a.py','python',10);"
        "INSERT INTO import_resolution(file_id, module_spec, resolved_file_id, status)"
        " VALUES ('a.py','b','b.py','resolved');");

    auto gbr = GraphBuilder::build(db, AlgoConfig{});
    EXPECT_EQ(gbr.file_graph.size(), 1);
    EXPECT_EQ(gbr.file_graph.id_to_node.count("b.py"), 0u);

    sqlite3_close(db);
}

TEST(GraphBuilderImports, EdgesKeepModuleSpecOrder)
{
    // add_edge appends, and every score is summed in adjacency order, so a
    // file's imports must go in in the order the old link table gave them:
    // by module spec, a target placed by its first spec -- not by target id.
    const char* kFiles =
        "INSERT INTO file(file_id, file_name, language, raw_loc) VALUES"
        " ('a.py','a.py','python',10), ('b.py','b.py','python',10),"
        " ('c.py','c.py','python',10), ('d.py','d.py','python',10);";
    auto targets = [](const Graph& g) {
        std::vector<std::string> out;
        for (NodeId v : g.adj[g.id_to_node.at("a.py")]) out.push_back(g.node_to_id[v]);
        return out;
    };

    // Spec order: .b, .d, c, d -- so b, d, c; by target id it would be b, c, d.
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);
    exec_sql(db, kFiles);
    exec_sql(db,
        "INSERT INTO import_resolution(file_id, module_spec, resolved_file_id, status) VALUES"
        " ('a.py','c','c.py','resolved'),"
        " ('a.py','d','d.py','resolved'),"
        " ('a.py','.d','d.py','resolved'),"
        " ('a.py','.b','b.py','resolved');");
    EXPECT_EQ(targets(GraphBuilder::build(db, AlgoConfig{}).file_graph),
              (std::vector<std::string>{"b.py", "d.py", "c.py"}));
    sqlite3_close(db);
}
