// Every function and class gets a reading_sequence row, linked or not.
// build_func_graph pre-populates a node per function_id/class_id, so an entity
// whose calls were all left unresolved -- or that has no links at all -- still
// gets a local_rank instead of vanishing from the sequence.

#include "algo/AlgoRunner.h"
#include "db/db.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// a.py: f calls g; lonely and the class C (with its method m) have no links.
// b.py: one function with no links, alone in its file.
class IsolatedDb {
public:
    IsolatedDb() {
        path_ = (fs::temp_directory_path() / "ts_isolated_entities_test.db").string();
        remove();
        sqlite3* db = nullptr;
        EXPECT_EQ(initDb(path_.c_str(), &db), SQLITE_OK);
        EXPECT_EQ(sqlite3_exec(db,
            "INSERT INTO file(file_id, file_name, raw_loc) VALUES ('a.py', 'a.py', 20), ('b.py', 'b.py', 3);"
            "INSERT INTO class(class_id, file_id, class_name, start_line, end_line) VALUES"
            " ('a.py::C', 'a.py', 'C', 10, 14);"
            "INSERT INTO function(function_id, file_id, class_id, function_name, start_line, end_line) VALUES"
            " ('a.py::f', 'a.py', NULL, 'f', 0, 2),"
            " ('a.py::g', 'a.py', NULL, 'g', 3, 5),"
            " ('a.py::lonely', 'a.py', NULL, 'lonely', 6, 8),"
            " ('a.py::C::m', NULL, 'a.py::C', 'm', 11, 14),"
            " ('b.py::solo', 'b.py', NULL, 'solo', 0, 2);"
            "INSERT INTO link(source_id, target_id, link_type) VALUES ('a.py::f', 'a.py::g', 'CALLS');",
            nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }
    ~IsolatedDb() { remove(); }

    const char* path() const { return path_.c_str(); }

private:
    void remove() {
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::error_code ec;
            fs::remove(path_ + suffix, ec);
        }
    }
    std::string path_;
};

}  // namespace

TEST(IsolatedEntities, EveryFunctionAndClassIsSequenced)
{
    IsolatedDb db;
    const AlgoRunResult r = AlgoRunner::run(db.path(), AlgoConfig{});

    std::map<std::string, const ReadingEntry*> byId;
    for (const ReadingEntry& e : r.entries) byId[e.entity_id] = &e;

    for (const char* id : {"a.py::f", "a.py::g", "a.py::lonely", "a.py::C", "a.py::C::m", "b.py::solo"}) {
        ASSERT_TRUE(byId.count(id)) << id;
        const ReadingEntry& e = *byId[id];
        EXPECT_GE(e.local_rank, 1) << id;
        EXPECT_TRUE(std::isfinite(e.pagerank_score) && std::isfinite(e.bc_score) &&
                    std::isfinite(e.combined_score)) << id;
    }
    EXPECT_EQ(byId["a.py::C"]->entity_type, "class");
    EXPECT_EQ(byId["a.py::lonely"]->entity_type, "function");
    EXPECT_EQ(byId["b.py::solo"]->local_rank, 1);

    // local_rank is 1..n within each file, no gaps or repeats.
    std::map<std::string, std::set<int>> ranks;
    std::map<std::string, int> count;
    for (const ReadingEntry& e : r.entries) {
        if (e.entity_type == "file") continue;
        ranks[e.file_id].insert(e.local_rank);
        ++count[e.file_id];
    }
    for (const auto& [file, set] : ranks) {
        EXPECT_EQ(static_cast<int>(set.size()), count[file]) << file;
        EXPECT_EQ(*set.begin(), 1) << file;
        EXPECT_EQ(*set.rbegin(), count[file]) << file;
    }

    // And all of it reaches the table.
    sqlite3* conn = nullptr;
    ASSERT_EQ(sqlite3_open(db.path(), &conn), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(conn,
        "SELECT COUNT(*) FROM reading_sequence WHERE entity_type <> 'file' AND local_rank >= 1;",
        -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 6);
    sqlite3_finalize(stmt);
    sqlite3_close(conn);
}

TEST(IsolatedEntities, OrderIsDeterministic)
{
    IsolatedDb db;
    auto order = [&] {
        std::vector<std::string> out;
        for (const ReadingEntry& e : AlgoRunner::run(db.path(), AlgoConfig{}).entries)
            out.push_back(e.entity_id + "#" + std::to_string(e.file_rank) + "#" +
                          std::to_string(e.local_rank));
        return out;
    };
    const auto first = order();
    EXPECT_EQ(order(), first);
}
