// tests/core/test_cd_builder.cpp
// BuildCDGraph's INHERITS edges (Step 6). The resolver writes INHERITS targets
// as class ids, so an edge joins on class_id: two classes that share a name in
// different files must not both receive the arrow.

#include "core/diagram/cd_builder.h"
#include "db/db.h"
#include "../link_rows.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

class TempDb {
public:
    explicit TempDb(const std::string& tag) {
        path_ = (fs::temp_directory_path() / ("ts_cd_builder_test_" + tag + ".db")).string();
        remove();
        EXPECT_EQ(initDb(path_.c_str(), &db_), SQLITE_OK);
    }
    ~TempDb() {
        if (db_) sqlite3_close(db_);
        remove();
    }

    sqlite3* db() const { return db_; }
    void exec(const char* sql) { ASSERT_EQ(sqlite3_exec(db_, sql, nullptr, nullptr, nullptr), SQLITE_OK) << sql; }
    void exec(const std::string& sql) { exec(sql.c_str()); }

private:
    void remove() {
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::error_code ec;
            fs::remove(path_ + suffix, ec);
        }
    }

    std::string path_;
    sqlite3* db_ = nullptr;
};

// Dependency edges as (source class_id, target class_id), sorted.
std::vector<std::pair<std::string, std::string>> dependencies(const TS::CDGraph& g)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const TS::CDEdge& e : g.edges)
        if (e.type == TS::CDEdgeType::Dependency)
            out.emplace_back(g.nodes[e.src_node_id].class_id, g.nodes[e.dst_node_id].class_id);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

// Two classes named B in different files; View's base is the one in models.
const char* kClasses =
    "INSERT INTO file(file_id, file_name, raw_loc) VALUES"
    " ('a/models.py', 'models.py', 1), ('a/helpers.py', 'helpers.py', 1), ('a/views.py', 'views.py', 1);"
    "INSERT INTO class(class_id, file_id, class_name, start_line, end_line) VALUES"
    " ('a/models.py::B', 'a/models.py', 'B', 0, 1),"
    " ('a/helpers.py::B', 'a/helpers.py', 'B', 0, 1),"
    " ('a/views.py::View', 'a/views.py', 'View', 0, 1),"
    " ('a/views.py::Other', 'a/views.py', 'Other', 2, 3);";

TEST(CDBuilderInherits, EdgeGoesToTheResolvedClassOnly)
{
    TempDb db("inherits");
    db.exec(kClasses);
    db.exec(test_links::linkRowsSql({{"a/views.py::View", "a/models.py::B", "INHERITS"}}));
    // An external base is a ref with no link at all.
    db.exec("INSERT INTO ref(file_id, source_id, scope_id, kind, name_path, raw_text, line, status)"
            " VALUES ('a/views.py', 'a/views.py::Other', 'a/views.py', 'INHERITS', 'ext.B', 'ext.B', 2,"
            "         'external');");

    const TS::CDGraph g = TS::BuildCDGraph(db.db());
    EXPECT_EQ(dependencies(g),
              (std::vector<std::pair<std::string, std::string>>{
                  {"a/views.py::View", "a/models.py::B"},
              }));
}

// Step 5 reads CALLS out of the view, joined on function ids at both ends.
TEST(CDBuilderCalls, MethodCallsBetweenClassesAreAssociations)
{
    TempDb db("calls");
    db.exec(kClasses);
    db.exec("INSERT INTO function(function_id, file_id, class_id, function_name, start_line, end_line) VALUES"
            " ('a/views.py::View::show', NULL, 'a/views.py::View', 'show', 0, 1),"
            " ('a/views.py::View::helper', NULL, 'a/views.py::View', 'helper', 0, 1),"
            " ('a/models.py::B::run', NULL, 'a/models.py::B', 'run', 0, 1),"
            " ('a/views.py::main', 'a/views.py', NULL, 'main', 4, 5);");
    db.exec(test_links::linkRowsSql({
        {"a/views.py::View::show", "a/models.py::B::run", "CALLS"},
        {"a/views.py::View::show", "a/views.py::View::helper", "CALLS"},   // same class
        {"a/views.py::main", "a/models.py::B::run", "CALLS"},              // no class
    }));

    const TS::CDGraph g = TS::BuildCDGraph(db.db());
    std::vector<std::pair<std::string, std::string>> associations;
    for (const TS::CDEdge& e : g.edges)
        if (e.type == TS::CDEdgeType::Association)
            associations.emplace_back(g.nodes[e.src_node_id].class_id, g.nodes[e.dst_node_id].class_id);
    EXPECT_EQ(associations,
              (std::vector<std::pair<std::string, std::string>>{
                  {"a/views.py::View", "a/models.py::B"},
              }));
}
