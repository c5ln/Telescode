#include "db.h"

#include <sqlite3.h>
#include <cstdio>

static const char* kInitSQL =
    "PRAGMA journal_mode = WAL;"
    "PRAGMA foreign_keys = ON;"
    "PRAGMA synchronous = NORMAL;"

    "CREATE TABLE IF NOT EXISTS file ("
    "    file_id   TEXT PRIMARY KEY,"
    "    file_name TEXT NOT NULL,"
    "    path      TEXT NOT NULL,"
    "    language  TEXT NOT NULL,"
    "    loc       INTEGER NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS class ("
    "    class_id   TEXT PRIMARY KEY,"
    "    file_id    TEXT NOT NULL REFERENCES file(file_id) ON DELETE CASCADE,"
    "    class_name TEXT NOT NULL,"
    "    start_line INTEGER NOT NULL,"
    "    end_line   INTEGER NOT NULL,"
    "    UNIQUE(file_id, class_name)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_class_file ON class(file_id);"

    "CREATE TABLE IF NOT EXISTS base_class ("
    "    class_id        TEXT NOT NULL REFERENCES class(class_id) ON DELETE CASCADE,"
    "    base_class_name TEXT NOT NULL,"
    "    ordinal         INTEGER NOT NULL,"
    "    PRIMARY KEY (class_id, ordinal)"
    ");"

    "CREATE TABLE IF NOT EXISTS function ("
    "    function_id   TEXT PRIMARY KEY,"
    "    file_id       TEXT REFERENCES file(file_id) ON DELETE CASCADE,"
    "    class_id      TEXT REFERENCES class(class_id) ON DELETE CASCADE,"
    "    function_name TEXT NOT NULL,"
    "    nesting_depth INTEGER NOT NULL DEFAULT 0,"
    "    is_async      INTEGER NOT NULL DEFAULT 0,"
    "    start_line    INTEGER NOT NULL,"
    "    end_line      INTEGER NOT NULL,"
    "    CHECK((file_id IS NULL) != (class_id IS NULL))"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_function_file  ON function(file_id)  WHERE file_id  IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_function_class ON function(class_id) WHERE class_id IS NOT NULL;"

    "CREATE TABLE IF NOT EXISTS param ("
    "    function_id TEXT NOT NULL REFERENCES function(function_id) ON DELETE CASCADE,"
    "    param_name  TEXT NOT NULL,"
    "    ordinal     INTEGER NOT NULL,"
    "    PRIMARY KEY (function_id, ordinal)"
    ");"

    "CREATE TABLE IF NOT EXISTS link ("
    "    source_id TEXT NOT NULL,"
    "    target_id TEXT NOT NULL,"
    "    link_type TEXT NOT NULL CHECK(link_type IN ('CALLS', 'INHERITS', 'IMPORTS', 'DECORATES')),"
    "    PRIMARY KEY (source_id, target_id, link_type)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_link_source ON link(source_id);"
    "CREATE INDEX IF NOT EXISTS idx_link_target ON link(target_id);"
    "CREATE INDEX IF NOT EXISTS idx_link_type   ON link(link_type);";

int initDb(const char* dbPath, sqlite3** db)
{
    if (!dbPath || !db) return SQLITE_MISUSE;
    *db = nullptr;

    int rc = sqlite3_open(dbPath, db);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "initDb: cannot open '%s': %s\n",
                     dbPath, *db ? sqlite3_errmsg(*db) : "unknown error");
        sqlite3_close(*db);
        *db = nullptr;
        return rc;
    }

    char* errMsg = nullptr;
    rc = sqlite3_exec(*db, kInitSQL, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "initDb: schema init failed: %s\n", errMsg);
        sqlite3_free(errMsg);
        sqlite3_close(*db);
        *db = nullptr;
        return rc;
    }

    return SQLITE_OK;
}
