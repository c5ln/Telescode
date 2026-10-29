// tests/core/test_cd_builder.cpp
// BuildCDGraph's INHERITS edges (Step 6). The resolver writes INHERITS targets
// as class ids, so an edge joins on class_id: two classes that share a name in
// different files must not both receive the arrow.

#include "core/diagram/cd_builder.h"
#include "db/db.h"

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

TEST(CDBuilderInherits, EdgeGoesToTheResolvedClassOnly)
{
    TempDb db("inherits");
    db.exec(
        "INSERT INTO file(file_id, file_name, raw_loc) VALUES"
        " ('a/models.py', 'models.py', 1), ('a/helpers.py', 'helpers.py', 1), ('a/views.py', 'views.py', 1);"
        "INSERT INTO class(class_id, file_id, class_name, start_line, end_line) VALUES"
        " ('a/models.py::B', 'a/models.py', 'B', 0, 1),"
        " ('a/helpers.py::B', 'a/helpers.py', 'B', 0, 1),"
        " ('a/views.py::View', 'a/views.py', 'View', 0, 1),"
        " ('a/views.py::Other', 'a/views.py', 'Other', 2, 3);"
        "INSERT INTO link(source_id, target_id, link_type) VALUES"
        " ('a/views.py::View', 'a/models.py::B', 'INHERITS'),"
        // An external base has no link at all; a name-only target, as an
        // old scan wrote, matches no class id and draws nothing.
        " ('a/views.py::Other', 'B', 'INHERITS');");

    const TS::CDGraph g = TS::BuildCDGraph(db.db());
    EXPECT_EQ(dependencies(g),
              (std::vector<std::pair<std::string, std::string>>{
                  {"a/views.py::View", "a/models.py::B"},
              }));
}
