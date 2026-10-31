#include "db.h"

#include <sqlite3.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#ifndef _WIN32
#include <stdlib.h>   // mkdtemp
#endif
#include <string>
#include <system_error>

// Per connection; none of these is stored in the file except journal_mode.
static const char* kPragmaSQL =
    "PRAGMA journal_mode = WAL;"
    "PRAGMA foreign_keys = ON;"
    "PRAGMA synchronous = NORMAL;"
    // 64 MiB page cache. A scan writes one transaction of tens of MB; with the
    // 2 MiB default the index pages spill mid-transaction and the
    // symbol/binding/ref inserts run ~1.7x slower.
    "PRAGMA cache_size = -65536;";

// Only ever run on an empty database (createSchema).
static const char* kSchemaSQL =
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

    "CREATE TABLE IF NOT EXISTS field ("
    "    class_id   TEXT NOT NULL REFERENCES class(class_id) ON DELETE CASCADE,"
    "    field_name TEXT NOT NULL,"
    "    access     TEXT NOT NULL DEFAULT '+',"
    "    PRIMARY KEY (class_id, field_name)"
    ");"

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

    // ── Name resolution ──────────────────────────────────────────────────────
    // The parser fills symbol/scope/binding/ref and the module specs of
    // import_resolution per file; the resolver fills in what each import and
    // ref resolves to. Every row
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
    // among them is the resolver's job. is_exported: as symbol.is_exported, for
    // module-level bindings -- what a wildcard import of the module sees.
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
    "    is_exported   INTEGER NOT NULL DEFAULT 1,"
    "    PRIMARY KEY (scope_id, namespace, local_name, line)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_binding_file ON binding(file_id);"

    // module_spec as written in file_id -> the project file it names. One row
    // per spec any import statement of the file names, whatever it binds:
    // `import pkg.h, pkg.i` binds `pkg` once but imports both. 'pending' until
    // the resolver has looked at it.
    "CREATE TABLE IF NOT EXISTS import_resolution ("
    "    file_id          TEXT NOT NULL REFERENCES file(file_id) ON DELETE CASCADE,"
    "    module_spec      TEXT NOT NULL,"
    "    resolved_file_id TEXT,"
    "    status           TEXT NOT NULL DEFAULT 'pending',"
    "    PRIMARY KEY (file_id, module_spec)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_import_resolution_target ON import_resolution(resolved_file_id);"

    // An unresolved use of a name. resolved_id has no FK: it may point into a
    // file that is re-scanned later, and the resolver re-checks it then.
    // emits_link: 1 on the ref that stands for a `link` row (kLinkViewSQL) --
    // resolved to a project symbol of the kind its link type takes -- and only
    // on the first such ref (lowest ref_id) of each link, so the view needs no
    // DISTINCT. Written by the resolver; idx_ref_link indexes these refs.
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
    "    emits_link  INTEGER NOT NULL DEFAULT 0,"
    "    UNIQUE(source_id, kind, name_path, line)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_ref_file     ON ref(file_id);"
    "CREATE INDEX IF NOT EXISTS idx_ref_status   ON ref(status);"
    "CREATE INDEX IF NOT EXISTS idx_ref_resolved ON ref(resolved_id);"
    "CREATE INDEX IF NOT EXISTS idx_ref_link     ON ref(kind, ref_id, source_id, resolved_id)"
    "    WHERE emits_link = 1;";

// `link`: every CALLS / INHERITS / DECORATES / IMPORTS edge, derived from the
// resolver's answers rather than stored beside them. One row per distinct
// (source_id, target_id, link_type), as the old table's primary key had it.
//   CALLS / INHERITS -- source symbol -> the symbol its ref resolved to
//   DECORATES        -- turned around: the ref runs decorated -> decorator,
//                       the link decorator -> decorated
//   IMPORTS          -- importing file -> imported file, for each module spec
//                       that names a project file. A file importing itself
//                       (`from . import x` in a package's __init__) is kept.
// Which refs are links (CALLS only to functions and methods, INHERITS only to
// classes, DECORATES to any symbol) the resolver decides and records in
// ref.emits_link, one ref per link. idx_ref_link covers the two ref arms in
// (kind, ref_id) order, so a reader filtering on link_type reads only that
// type's slice. A reader that needs a fixed row order (Graph) reads ref or
// import_resolution itself, with an ORDER BY the view cannot express.
static const char* kLinkViewSQL =
    "CREATE VIEW IF NOT EXISTS link(source_id, target_id, link_type) AS"
    "  SELECT source_id, resolved_id, kind FROM ref"
    "   WHERE emits_link = 1 AND kind IN ('CALLS', 'INHERITS')"
    "  UNION ALL"
    "  SELECT resolved_id, source_id, kind FROM ref"
    "   WHERE emits_link = 1 AND kind = 'DECORATES'"
    "  UNION ALL"
    "  SELECT DISTINCT file_id, resolved_file_id, 'IMPORTS' FROM import_resolution"
    "   WHERE status = 'resolved';";

int createSchema(sqlite3* db)
{
    const std::string sql = std::string(kSchemaSQL) + kLinkViewSQL +
                            "PRAGMA user_version = " + std::to_string(kDbVersion) + ";";
    char* errMsg = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "createSchema: %s\n", errMsg);
        sqlite3_free(errMsg);
    }
    return rc;
}

namespace {

// What is at a database path.
enum class DbKind {
    Empty,          // nothing there, a 0-byte file, or SQLite with nothing in it
    Current,        // user_version == kDbVersion
    OtherVersion,   // a Telescode database of another version: has `file` and `link`
    NotTelescode,   // anything else, SQLite or not
};

// Classifies the database an open handle sees. Reads only.
int classify(sqlite3* db, DbKind* kind)
{
    // One statement, so every answer comes from the same snapshot. `file` and
    // `link` of any type: before Stage 4 link was a table, and before
    // user_version the version was 0.
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "SELECT (SELECT user_version FROM pragma_user_version),"
        "       EXISTS(SELECT 1 FROM sqlite_master),"
        "       (SELECT COUNT(DISTINCT name) FROM sqlite_master WHERE name IN ('file', 'link')) = 2;",
        -1, &stmt, nullptr);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_NOTADB || rc == SQLITE_CORRUPT) {
            *kind = DbKind::NotTelescode;
            return SQLITE_OK;
        }
        return rc;
    }
    const int version = sqlite3_column_int(stmt, 0);
    const bool hasObjects = sqlite3_column_int(stmt, 1) != 0;
    const bool telescode = sqlite3_column_int(stmt, 2) != 0;
    sqlite3_finalize(stmt);

    *kind = version == kDbVersion           ? DbKind::Current
          : version == 0 && !hasObjects     ? DbKind::Empty
          : telescode                       ? DbKind::OtherVersion
                                            : DbKind::NotTelescode;
    return SQLITE_OK;
}

// file: URI for path, with `query` appended.
std::string uriOf(const std::string& path, const char* query)
{
    // An absolute path gets an empty authority ("file://" + "/x"): with plain
    // "file:" a path starting "//" would have its first segment read as one.
    std::string uri = !path.empty() && path[0] == '/' ? "file://" : "file:";
    for (char c : path) {
        if (c == '%' || c == '?' || c == '#') {
            char esc[4];
            std::snprintf(esc, sizeof esc, "%%%02X", static_cast<unsigned char>(c));
            uri += esc;
        } else {
            uri += c;
        }
    }
    return uri + '?' + query;
}

int classifyUri(const std::string& uri, DbKind* kind)
{
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
    if (rc == SQLITE_OK) rc = classify(db, kind);
    sqlite3_close(db);
    return rc;
}

// Classifies the database at dbPath without changing or creating any file --
// not the main file, not its -wal or -shm. A plain read-only open would: it
// creates -wal / -shm for a WAL database, writes read marks into -shm, and a
// read-write one checkpoints the WAL into the main file on close.
int classifyPath(const char* dbPath, DbKind* kind)
{
    namespace fs = std::filesystem;
    const std::string path = dbPath;
    if (path == ":memory:") { *kind = DbKind::Empty; return SQLITE_OK; }

    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size == 0) { *kind = DbKind::Empty; return SQLITE_OK; }

    {
        char header[16] = {};
        std::ifstream in(path, std::ios::binary);
        in.read(header, sizeof header);
        if (in.gcount() != sizeof header ||
            std::memcmp(header, "SQLite format 3", sizeof header) != 0) {
            *kind = DbKind::NotTelescode;
            return SQLITE_OK;
        }
    }

    const std::string wal = path + "-wal";
    const auto walSize = fs::file_size(wal, ec);
    if (ec || walSize == 0) {
        // No WAL content: the main file is the whole database. immutable
        // takes no locks and opens no other file.
        return classifyUri(uriOf(path, "mode=ro&immutable=1"), kind);
    }
    if (fs::exists(path + "-shm", ec)) {
        // readonly_shm: the -shm is opened read-only, so SQLite reads the WAL
        // itself into private memory instead of updating the shared index.
        return classifyUri(uriOf(path, "mode=ro&readonly_shm=1"), kind);
    }

    // A WAL with no -shm (deleted by hand, say): SQLite cannot read it without
    // creating one, so look at a copy instead, in a directory only we can read.
    // Nothing short of reading the WAL settles the case: it can change
    // user_version and the schema both, whatever the main file says.
    const fs::path tmp = fs::temp_directory_path(ec);
    if (ec) return SQLITE_CANTOPEN;
#ifndef _WIN32
    std::string templ = (tmp / "telescode-check-XXXXXX").string();
    if (!mkdtemp(templ.data())) return SQLITE_CANTOPEN;   // created 0700
    const fs::path dir = templ;
#else
    const fs::path dir = tmp / ("telescode-check-" + std::to_string(std::random_device{}()));
    if (!fs::create_directory(dir, ec) || ec) return SQLITE_CANTOPEN;
#endif
    const std::string copy = (dir / "db").string();
    int rc = SQLITE_CANTOPEN;
    if (fs::copy_file(path, copy, ec) && fs::copy_file(wal, copy + "-wal", ec)) {
        sqlite3* db = nullptr;
        rc = sqlite3_open(copy.c_str(), &db);
        if (rc == SQLITE_OK) rc = classify(db, kind);
        sqlite3_close(db);
    }
    fs::remove_all(dir, ec);
    return rc;
}

// sqlite3_open plus the pragmas, for a path classifyPath has already passed.
// On failure *db is closed and null.
int openReadWrite(const char* dbPath, sqlite3** db)
{
    int rc = sqlite3_open(dbPath, db);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "initDb: cannot open '%s': %s\n",
                     dbPath, *db ? sqlite3_errmsg(*db) : "unknown error");
    } else {
        char* errMsg = nullptr;
        rc = sqlite3_exec(*db, kPragmaSQL, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::fprintf(stderr, "initDb: %s\n", errMsg);
            sqlite3_free(errMsg);
        }
    }
    if (rc != SQLITE_OK) {
        sqlite3_close(*db);
        *db = nullptr;
    }
    return rc;
}

}  // namespace

std::string dbOpenError(const std::string& dbPath, int rc)
{
    if (rc == kNotADatabase) return dbPath + ' ' + kNotADatabaseMessage;
    return dbPath + ": " + (rc == kWrongDbVersion ? kWrongDbVersionMessage : sqlite3_errstr(rc));
}

int checkDbVersion(const char* dbPath, bool* empty)
{
    if (empty) *empty = false;
    if (!dbPath) return SQLITE_MISUSE;
    DbKind kind;
    const int rc = classifyPath(dbPath, &kind);
    if (rc != SQLITE_OK) return rc;
    if (kind == DbKind::Empty && empty) *empty = true;
    return kind == DbKind::Current || kind == DbKind::Empty ? SQLITE_OK
         : kind == DbKind::OtherVersion                     ? kWrongDbVersion
                                                            : kNotADatabase;
}

int initDb(const char* dbPath, sqlite3** db)
{
    if (!dbPath || !db) return SQLITE_MISUSE;
    *db = nullptr;

    bool empty = false;
    int rc = checkDbVersion(dbPath, &empty);
    if (rc == SQLITE_OK) rc = openReadWrite(dbPath, db);
    if (rc != SQLITE_OK || !empty) return rc;

    // Looked at again under the write lock: another process may have filled
    // the file since. IF NOT EXISTS in the schema covers the rest.
    DbKind kind = DbKind::Empty;
    rc = sqlite3_exec(*db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
    if (rc == SQLITE_OK) rc = classify(*db, &kind);
    if (rc == SQLITE_OK && kind == DbKind::Empty) rc = createSchema(*db);
    else if (rc == SQLITE_OK && kind == DbKind::OtherVersion) rc = kWrongDbVersion;
    else if (rc == SQLITE_OK && kind != DbKind::Current) rc = kNotADatabase;
    if (rc == SQLITE_OK) rc = sqlite3_exec(*db, "COMMIT;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_exec(*db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(*db);
        *db = nullptr;
    }
    return rc;
}

int openDbForScan(const char* dbPath, sqlite3** db, bool* empty)
{
    if (!dbPath || !db || !empty) return SQLITE_MISUSE;
    *db = nullptr;
    *empty = false;

    DbKind kind;
    int rc = classifyPath(dbPath, &kind);
    if (rc != SQLITE_OK) return rc;
    // Only ever delete a Telescode database: anything else at the path (a
    // mistyped source file, someone else's SQLite file) may be the only copy.
    if (kind == DbKind::NotTelescode) return kNotADatabase;

    if (kind == DbKind::OtherVersion) {
        // Start over from nothing. The -wal file goes too, or SQLite would
        // replay the old database's pages into the new one.
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::error_code ec;
            std::filesystem::remove(std::string(dbPath) + suffix, ec);
            if (ec) {
                std::fprintf(stderr, "scan: cannot remove '%s%s': %s\n",
                             dbPath, suffix, ec.message().c_str());
                return SQLITE_CANTOPEN;
            }
        }
    }

    rc = openReadWrite(dbPath, db);
    if (rc != SQLITE_OK) return rc;
    // Again on the read-write handle: only another process writing the file
    // between the two looks can make this differ from classifyPath.
    rc = classify(*db, &kind);
    if (rc == SQLITE_OK && kind != DbKind::Current && kind != DbKind::Empty) rc = kNotADatabase;
    if (rc != SQLITE_OK) {
        sqlite3_close(*db);
        *db = nullptr;
        return rc;
    }
    *empty = kind == DbKind::Empty;
    return SQLITE_OK;
}
