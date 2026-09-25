// tests/core/test_analysis_readonly.cpp
// AnalysisService::Load must not write to the database unless asked to.
//
// Without run_algo, Load is the engine behind `graph`, `sequence` and `analyze`,
// all of which are documented as reporting the database as it stands. The way
// that guarantee gets lost is indirect: reading the stored AlgoConfig goes
// through initDb, which opens read-write and will create tables, migrate the
// schema, switch journal_mode to WAL, and create the file if it is missing. So
// these tests assert on the schema and journal mode, not just on row counts --
// a migration moves neither.

#include "core/AnalysisService.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// A database fixture that deletes itself, sidecars included.
class TempDb {
public:
    explicit TempDb(const std::string& tag) {
        path_ = (fs::temp_directory_path() /
                 ("ts_ro_test_" + tag + ".db")).string();
        remove();
    }
    ~TempDb() { remove(); }

    const std::string& path() const { return path_; }

    void exec(const char* sql) {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(path_.c_str(), &db), SQLITE_OK);
        char* err = nullptr;
        const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            const std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            sqlite3_close(db);
            FAIL() << "setup SQL failed: " << msg;
        }
        sqlite3_close(db);
    }

    // Everything that a create/migrate pass would disturb.
    std::string fingerprint() const {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(path_.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            return "<unopenable>";
        }
        std::string out;
        collect(db, "SELECT type||'|'||name||'|'||COALESCE(sql,'') "
                    "FROM sqlite_master ORDER BY type,name;", out);
        collect(db, "PRAGMA journal_mode;", out);
        collect(db, "PRAGMA schema_version;", out);
        sqlite3_close(db);
        return out;
    }

    bool exists() const { return fs::exists(path_); }

private:
    static void collect(sqlite3* db, const char* sql, std::string& out) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(stmt, 0);
            out += t ? reinterpret_cast<const char*>(t) : "";
            out += '\n';
        }
        sqlite3_finalize(stmt);
    }

    void remove() const {
        std::error_code ec;
        fs::remove(path_, ec);
        fs::remove(path_ + "-wal", ec);
        fs::remove(path_ + "-shm", ec);
    }

    std::string path_;
};

// The shape a database had before the rollup-metric columns existed: file.loc
// rather than raw_loc, and no algo tables. initDb would rename the column, add
// nine more, and create the missing tables.
const char* const kLegacySchema =
    "PRAGMA journal_mode = DELETE;"
    "CREATE TABLE file ("
    "  file_id TEXT PRIMARY KEY, file_name TEXT NOT NULL,"
    "  language TEXT NOT NULL, loc INTEGER NOT NULL);"
    "INSERT INTO file VALUES ('a.py','a.py','python',10);";

} // anonymous namespace

TEST(AnalysisReadOnly, LegacySchemaIsNotMigrated) {
    TempDb db("legacy");
    db.exec(kLegacySchema);
    const std::string before = db.fingerprint();
    ASSERT_NE(before.find("loc INTEGER NOT NULL"), std::string::npos)
        << "fixture should start on the pre-migration schema";

    TS::AnalysisOptions opts;   // run_algo stays false
    EXPECT_NO_THROW(TS::AnalysisService::Load(db.path(), opts));

    EXPECT_EQ(db.fingerprint(), before)
        << "a read-only analysis migrated the schema";
}

TEST(AnalysisReadOnly, MissingAlgoTablesAreNotCreated) {
    TempDb db("notables");
    db.exec("CREATE TABLE file ("
            "  file_id TEXT PRIMARY KEY, file_name TEXT NOT NULL,"
            "  language TEXT NOT NULL, raw_loc INTEGER NOT NULL,"
            "  logical_loc INTEGER NOT NULL DEFAULT 0,"
            "  is_generated INTEGER NOT NULL DEFAULT 0,"
            "  max_cyclomatic_complexity INTEGER NOT NULL DEFAULT 0,"
            "  avg_cyclomatic_complexity REAL NOT NULL DEFAULT 0.0,"
            "  max_block_depth INTEGER NOT NULL DEFAULT 0,"
            "  avg_block_depth REAL NOT NULL DEFAULT 0.0,"
            "  max_function_loc INTEGER NOT NULL DEFAULT 0,"
            "  avg_function_loc REAL NOT NULL DEFAULT 0.0,"
            "  complexity_score REAL NOT NULL DEFAULT 0.0);");
    const std::string before = db.fingerprint();
    ASSERT_EQ(before.find("reading_sequence"), std::string::npos)
        << "fixture should start without the algo tables";

    TS::AnalysisOptions opts;
    const TS::AnalysisSnapshot snap = TS::AnalysisService::Load(db.path(), opts);
    // A database whose algo pass never ran reports an empty sequence, not an error.
    EXPECT_TRUE(snap.reading_sequence.empty());

    EXPECT_EQ(db.fingerprint(), before)
        << "a read-only analysis created tables";
}

TEST(AnalysisReadOnly, JournalModeIsNotSwitchedToWal) {
    TempDb db("journal");
    db.exec(kLegacySchema);   // sets journal_mode = DELETE
    const std::string before = db.fingerprint();
    ASSERT_NE(before.find("delete"), std::string::npos)
        << "fixture should start outside WAL mode";

    TS::AnalysisOptions opts;
    TS::AnalysisService::Load(db.path(), opts);

    EXPECT_NE(db.fingerprint().find("delete"), std::string::npos)
        << "a read-only analysis switched the journal mode";
}

TEST(AnalysisReadOnly, MissingDatabaseFailsInsteadOfBeingCreated) {
    TempDb db("absent");   // constructor removes it; nothing creates it
    ASSERT_FALSE(db.exists());

    TS::AnalysisOptions opts;
    EXPECT_THROW(TS::AnalysisService::Load(db.path(), opts), std::runtime_error);

    // A mistyped path must not leave a new empty database behind, which would
    // then report a perfectly successful analysis of nothing.
    EXPECT_FALSE(db.exists());
}
