#include "db.h"

#include <sqlite3.h>
#include <cstdio>
#include <cstring>
#include <string>

static const char* kInitSQL =
    "PRAGMA journal_mode = WAL;"
    "PRAGMA foreign_keys = ON;"
    "PRAGMA synchronous = NORMAL;"
    // 64 MiB page cache (per connection, not stored in the file). A scan writes
    // one transaction of tens of MB; with the 2 MiB default the index pages
    // spill mid-transaction and the symbol/binding/ref inserts run ~1.7x slower.
    "PRAGMA cache_size = -65536;"

    "CREATE TABLE IF NOT EXISTS file ("
    "    file_id                   TEXT PRIMARY KEY,"
    "    file_name                 TEXT NOT NULL,"
    "    language                  TEXT NOT NULL DEFAULT 'python',"
    "    raw_loc                   INTEGER NOT NULL,"
    "    logical_loc               INTEGER NOT NULL DEFAULT 0,"
    "    is_generated              INTEGER NOT NULL DEFAULT 0,"
    "    max_cyclomatic_complexity INTEGER NOT NULL DEFAULT 0,"
    "    avg_cyclomatic_complexity REAL    NOT NULL DEFAULT 0.0,"
    "    max_block_depth           INTEGER NOT NULL DEFAULT 0,"
    "    avg_block_depth           REAL    NOT NULL DEFAULT 0.0,"
    "    max_function_loc          INTEGER NOT NULL DEFAULT 0,"
    "    avg_function_loc          REAL    NOT NULL DEFAULT 0.0,"
    "    complexity_score          REAL    NOT NULL DEFAULT 0.0,"
    "    module_name               TEXT"
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
    "    function_id            TEXT PRIMARY KEY,"
    "    file_id                TEXT REFERENCES file(file_id) ON DELETE CASCADE,"
    "    class_id               TEXT REFERENCES class(class_id) ON DELETE CASCADE,"
    "    function_name          TEXT NOT NULL,"
    "    nesting_depth          INTEGER NOT NULL DEFAULT 0,"
    "    is_async               INTEGER NOT NULL DEFAULT 0,"
    "    cyclomatic_complexity  INTEGER NOT NULL DEFAULT 1,"
    "    max_block_depth        INTEGER NOT NULL DEFAULT 0,"
    "    loc                    INTEGER NOT NULL DEFAULT 0,"
    "    start_line             INTEGER NOT NULL,"
    "    end_line               INTEGER NOT NULL,"
    "    CHECK ((file_id IS NULL) <> (class_id IS NULL))"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_function_file ON function(file_id) WHERE file_id IS NOT NULL;"
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

    "CREATE TABLE IF NOT EXISTS field ("
    "    class_id   TEXT NOT NULL REFERENCES class(class_id) ON DELETE CASCADE,"
    "    field_name TEXT NOT NULL,"
    "    access     TEXT NOT NULL DEFAULT '+',"
    "    PRIMARY KEY (class_id, field_name)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_link_source ON link(source_id);"
    "CREATE INDEX IF NOT EXISTS idx_link_target ON link(target_id);"
    "CREATE INDEX IF NOT EXISTS idx_link_type   ON link(link_type);"

    "CREATE TABLE IF NOT EXISTS reading_sequence ("
    "    entity_id      TEXT PRIMARY KEY,"
    "    entity_type    TEXT NOT NULL CHECK(entity_type IN ('file', 'class', 'function')),"
    "    file_id        TEXT NOT NULL REFERENCES file(file_id) ON DELETE CASCADE,"
    "    file_rank      INTEGER,"
    "    local_rank     INTEGER,"
    "    pagerank_score REAL NOT NULL,"
    "    bc_score       REAL NOT NULL,"
    "    combined_score REAL NOT NULL,"
    "    CHECK("
    "        (entity_type = 'file'  AND file_rank  IS NOT NULL AND local_rank IS NULL) OR"
    "        (entity_type != 'file' AND local_rank IS NOT NULL AND file_rank  IS NULL)"
    "    )"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_rs_file      ON reading_sequence(file_id);"
    "CREATE INDEX IF NOT EXISTS idx_rs_file_rank ON reading_sequence(file_rank);"

    "CREATE TABLE IF NOT EXISTS reading_sequence_config ("
    "    config_key   TEXT PRIMARY KEY,"
    "    config_value TEXT NOT NULL"
    ");"

    // ── Name resolution (schema version 2) ───────────────────────────────────
    // The parser fills symbol/scope/binding/ref per file; import_resolution and
    // the resolved_id/status of each ref are written by the resolver. Every row
    // carries its file_id so DbUpdater's per-file delete removes it by cascade.

    // One row per named definition. class/function stay as the metric tables and
    // share this PK 1:1. parent_id is the enclosing symbol, NULL at module level.
    "CREATE TABLE IF NOT EXISTS symbol ("
    "    symbol_id   TEXT PRIMARY KEY,"
    "    file_id     TEXT NOT NULL REFERENCES file(file_id) ON DELETE CASCADE,"
    "    parent_id   TEXT,"
    "    kind        TEXT NOT NULL CHECK(kind IN ('class', 'interface', 'struct', 'trait', 'enum',"
    "                                           'function', 'method', 'field', 'variable',"
    "                                           'type_alias', 'namespace')),"
    "    name        TEXT NOT NULL,"
    "    namespace   TEXT NOT NULL DEFAULT 'value' CHECK(namespace IN ('value', 'type')),"
    "    is_exported INTEGER NOT NULL DEFAULT 1,"
    "    signature   TEXT,"
    "    start_line  INTEGER NOT NULL,"
    "    end_line    INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_symbol_file   ON symbol(file_id);"
    "CREATE INDEX IF NOT EXISTS idx_symbol_parent ON symbol(parent_id, name);"

    // Lexical scopes. The file scope's id is the file_id; a class or function
    // scope's id is its symbol id, which is also owner_id.
    "CREATE TABLE IF NOT EXISTS scope ("
    "    scope_id        TEXT PRIMARY KEY,"
    "    file_id         TEXT NOT NULL REFERENCES file(file_id) ON DELETE CASCADE,"
    "    parent_scope_id TEXT,"
    "    kind            TEXT NOT NULL CHECK(kind IN ('file', 'namespace', 'class', 'function', 'block')),"
    "    owner_id        TEXT,"
    "    start_line      INTEGER NOT NULL,"
    "    end_line        INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_scope_file ON scope(file_id);"

    // What a name means inside one scope. target_id is the defining symbol for
    // `def`, NULL for a local with no symbol row (parameters, function locals).
    // A name bound more than once in a scope has one row per line; choosing
    // among them is the resolver's job.
    "CREATE TABLE IF NOT EXISTS binding ("
    "    scope_id      TEXT NOT NULL,"
    "    local_name    TEXT NOT NULL,"
    "    namespace     TEXT NOT NULL DEFAULT 'value' CHECK(namespace IN ('value', 'type')),"
    "    kind          TEXT NOT NULL CHECK(kind IN ('def', 'import_module', 'import_symbol', 'import_all', 'reexport')),"
    "    file_id       TEXT NOT NULL REFERENCES file(file_id) ON DELETE CASCADE,"
    "    target_id     TEXT,"
    "    module_spec   TEXT,"
    "    imported_name TEXT,"
    "    line          INTEGER NOT NULL,"
    "    PRIMARY KEY (scope_id, namespace, local_name, line)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_binding_file ON binding(file_id);"

    // module_spec as written in file_id -> the project file it names.
    "CREATE TABLE IF NOT EXISTS import_resolution ("
    "    file_id          TEXT NOT NULL REFERENCES file(file_id) ON DELETE CASCADE,"
    "    module_spec      TEXT NOT NULL,"
    "    resolved_file_id TEXT,"
    "    status           TEXT NOT NULL,"
    "    PRIMARY KEY (file_id, module_spec)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_import_resolution_target ON import_resolution(resolved_file_id);"

    // An unresolved use of a name. resolved_id has no FK: it may point into a
    // file that is re-scanned later, and the resolver re-checks it then.
    "CREATE TABLE IF NOT EXISTS ref ("
    "    ref_id      INTEGER PRIMARY KEY,"
    "    file_id     TEXT NOT NULL REFERENCES file(file_id) ON DELETE CASCADE,"
    "    source_id   TEXT NOT NULL,"
    "    scope_id    TEXT NOT NULL,"
    "    kind        TEXT NOT NULL CHECK(kind IN ('CALLS', 'INHERITS', 'IMPLEMENTS', 'DECORATES')),"
    "    subkind     TEXT,"
    "    namespace   TEXT NOT NULL DEFAULT 'value' CHECK(namespace IN ('value', 'type')),"
    "    name_path   TEXT NOT NULL,"
    "    raw_text    TEXT NOT NULL,"
    "    line        INTEGER NOT NULL,"
    "    resolved_id TEXT,"
    "    status      TEXT NOT NULL DEFAULT 'pending'"
    "                CHECK(status IN ('pending', 'resolved', 'builtin', 'external', 'unresolved')),"
    "    confidence  TEXT CHECK(confidence IN ('exact', 'heuristic')),"
    "    resolver    TEXT,"
    "    UNIQUE(source_id, kind, name_path, line)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_ref_file     ON ref(file_id);"
    "CREATE INDEX IF NOT EXISTS idx_ref_status   ON ref(status);"
    "CREATE INDEX IF NOT EXISTS idx_ref_resolved ON ref(resolved_id);";

// Bumped whenever kInitSQL/migrateSchema change what a database holds.
//   2 -- symbol/scope/binding/import_resolution/ref, file.module_name
static const int kSchemaVersion = 2;

// SQLite has no "ADD COLUMN IF NOT EXISTS" — check via PRAGMA table_info first.
static bool columnExists(sqlite3* db, const char* table, const char* column)
{
    std::string sql = std::string("PRAGMA table_info(") + table + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name && std::strcmp(name, column) == 0) { found = true; break; }
    }
    sqlite3_finalize(stmt);
    return found;
}

// Adds one column unless the table already has it.
//
// Every column added this way must carry a DEFAULT. SQLite refuses to ADD a
// NOT NULL column without one -- the rows already in the table would have
// nothing to put in it -- and that refusal is what makes `raw_loc` below a
// rename rather than an add.
static int addColumnIfMissing(sqlite3* db, const char* table,
                              const char* column, const char* decl)
{
    if (columnExists(db, table, column)) return SQLITE_OK;

    const std::string sql =
        std::string("ALTER TABLE ") + table + " ADD COLUMN " + column + ' ' + decl + ';';

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "initDb: adding %s.%s failed: %s\n", table, column, errMsg);
        sqlite3_free(errMsg);
    }
    return rc;
}

// Brings a database written by an earlier version up to the current schema.
//
// kInitSQL creates tables with CREATE TABLE IF NOT EXISTS, so it leaves an
// existing table exactly as it found it. Without this pass an old `file` table
// keeps its original columns, and every statement in DbInserter -- which names
// the current ones -- fails to even prepare, surfacing as a bare
// "SQL logic error" from a scan that never touches a row.
//
// This restores the schema, not the data: the added columns hold their defaults
// until the repository is scanned again. Idempotent, and a no-op on a fresh
// database, where kInitSQL has already created every column.
static int migrateSchema(sqlite3* db)
{
    // `loc` was replaced by `raw_loc`, not supplemented by it, so a rename does
    // the work of two steps: it carries the old values over under the new name
    // with exactly the NOT NULL / no-default definition kInitSQL gives raw_loc,
    // and it retires `loc` -- which no current INSERT names, and which would
    // reject every one of them, being NOT NULL with no default of its own.
    if (columnExists(db, "file", "loc") && !columnExists(db, "file", "raw_loc")) {
        char* errMsg = nullptr;
        int rc = sqlite3_exec(db, "ALTER TABLE file RENAME COLUMN loc TO raw_loc;",
                              nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::fprintf(stderr, "initDb: renaming file.loc to raw_loc failed: %s\n", errMsg);
            sqlite3_free(errMsg);
            return rc;
        }
    }

    struct ColumnDef { const char* table; const char* column; const char* decl; };
    static const ColumnDef kColumns[] = {
        { "file",     "logical_loc",               "INTEGER NOT NULL DEFAULT 0"   },
        { "file",     "is_generated",              "INTEGER NOT NULL DEFAULT 0"   },
        { "file",     "max_cyclomatic_complexity", "INTEGER NOT NULL DEFAULT 0"   },
        { "file",     "avg_cyclomatic_complexity", "REAL NOT NULL DEFAULT 0.0"    },
        { "file",     "max_block_depth",           "INTEGER NOT NULL DEFAULT 0"   },
        { "file",     "avg_block_depth",           "REAL NOT NULL DEFAULT 0.0"    },
        { "file",     "max_function_loc",          "INTEGER NOT NULL DEFAULT 0"   },
        { "file",     "avg_function_loc",          "REAL NOT NULL DEFAULT 0.0"    },
        { "file",     "complexity_score",          "REAL NOT NULL DEFAULT 0.0"    },
        { "function", "cyclomatic_complexity",     "INTEGER NOT NULL DEFAULT 1"   },
        { "function", "max_block_depth",           "INTEGER NOT NULL DEFAULT 0"   },
        { "function", "loc",                       "INTEGER NOT NULL DEFAULT 0"   },
        { "file",     "module_name",               "TEXT"                         },
    };

    for (const ColumnDef& c : kColumns) {
        const int rc = addColumnIfMissing(db, c.table, c.column, c.decl);
        if (rc != SQLITE_OK) return rc;
    }

    // A version-1 database gets the version-2 tables from kInitSQL, but empty:
    // symbol/scope/binding/ref are only written by a scan, so name resolution
    // has nothing to work on until the repository is scanned again.
    const std::string version = "PRAGMA user_version = " + std::to_string(kSchemaVersion) + ";";
    return sqlite3_exec(db, version.c_str(), nullptr, nullptr, nullptr);
}

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

    rc = migrateSchema(*db);
    if (rc != SQLITE_OK) {
        sqlite3_close(*db);
        *db = nullptr;
        return rc;
    }

    return SQLITE_OK;
}
