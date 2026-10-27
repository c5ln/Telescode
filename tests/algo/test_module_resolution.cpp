#include "algo/Graph.h"
#include "db/db.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <string>

// Module resolution itself is tested in tests/lang/test_python_language.cpp.

// ── Through GraphBuilder ──────────────────────────────────────────────────────
// The PythonLanguage unit tests cannot catch a wiring mistake: resolveModule
// needs the importing file, and build_file_graph has to hand it the right one.

namespace {

void exec_sql(sqlite3* db, const char* sql)
{
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "");
    sqlite3_free(err);
}

bool has_edge(const Graph& g, const std::string& src, const std::string& tgt)
{
    auto u = g.id_to_node.find(src);
    auto v = g.id_to_node.find(tgt);
    if (u == g.id_to_node.end() || v == g.id_to_node.end()) return false;
    for (NodeId w : g.adj[u->second]) if (w == v->second) return true;
    return false;
}

}  // namespace

TEST(GraphBuilderImports, RelativeImportsBecomeEdges)
{
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);

    exec_sql(db,
        "INSERT INTO file(file_id, file_name, language, raw_loc) VALUES"
        " ('pkg/__init__.py','__init__.py','python',10),"
        " ('pkg/utils.py','utils.py','python',10),"
        " ('pkg/core/__init__.py','__init__.py','python',10),"
        " ('pkg/core/dataset.py','dataset.py','python',10),"
        " ('pkg/core/variable.py','variable.py','python',10),"
        " ('absmod.py','absmod.py','python',10);");

    exec_sql(db,
        "INSERT INTO link(source_id, target_id, link_type) VALUES"
        " ('pkg/__init__.py','.core','IMPORTS'),"
        " ('pkg/core/dataset.py','.variable','IMPORTS'),"
        " ('pkg/core/dataset.py','..utils','IMPORTS'),"
        " ('pkg/core/variable.py','.','IMPORTS'),"
        " ('absmod.py','pkg.core.dataset','IMPORTS'),"
        " ('absmod.py','os','IMPORTS'),"
        " ('pkg/core/dataset.py','...escape','IMPORTS');");

    auto gbr = GraphBuilder::build(db, AlgoConfig{});
    const Graph& g = gbr.file_graph;

    EXPECT_TRUE(has_edge(g, "pkg/__init__.py",      "pkg/core/__init__.py"));
    EXPECT_TRUE(has_edge(g, "pkg/core/dataset.py",  "pkg/core/variable.py"));
    EXPECT_TRUE(has_edge(g, "pkg/core/dataset.py",  "pkg/utils.py"));
    EXPECT_TRUE(has_edge(g, "pkg/core/variable.py", "pkg/core/__init__.py"));
    EXPECT_TRUE(has_edge(g, "absmod.py",            "pkg/core/dataset.py"));

    // `os` is external and `...escape` climbs above the repo root: neither adds
    // a node, so the graph holds exactly the six project files.
    EXPECT_EQ(g.id_to_node.count("os"), 0u);
    EXPECT_EQ(g.size(), 6);

    sqlite3_close(db);
}

TEST(GraphBuilderImports, SrcLayoutImportsBecomeEdges)
{
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);

    exec_sql(db,
        "INSERT INTO file(file_id, file_name, language, raw_loc) VALUES"
        " ('src/pytest.py','pytest.py','python',10),"
        " ('src/_pytest/__init__.py','__init__.py','python',10),"
        " ('src/_pytest/logging.py','logging.py','python',10),"
        " ('testing/test_logging.py','test_logging.py','python',10);");

    exec_sql(db,
        "INSERT INTO link(source_id, target_id, link_type) VALUES"
        " ('testing/test_logging.py','_pytest.logging','IMPORTS'),"
        " ('src/pytest.py','_pytest.logging','IMPORTS'),"
        " ('src/_pytest/logging.py','.','IMPORTS'),"
        " ('testing/test_logging.py','numpy','IMPORTS');");

    auto gbr = GraphBuilder::build(db, AlgoConfig{});
    const Graph& g = gbr.file_graph;

    EXPECT_TRUE(has_edge(g, "testing/test_logging.py", "src/_pytest/logging.py"));
    EXPECT_TRUE(has_edge(g, "src/pytest.py",           "src/_pytest/logging.py"));
    EXPECT_TRUE(has_edge(g, "src/_pytest/logging.py",  "src/_pytest/__init__.py"));
    EXPECT_EQ(g.id_to_node.count("numpy"), 0u);

    sqlite3_close(db);
}
