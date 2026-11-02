#include "db.h"

#include <sqlite3.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#ifndef _WIN32
#include <stdlib.h>   // mkdtemp
#include <unistd.h>   // access
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

// Only ever run on an empty database (createSchema). The tables only: their
// secondary indexes are kIndexSQL, which a scan builds once the rows are in.
// A CHECK lists its values with OR, not IN: SQLite builds an IN list of more
// than two values into a temporary b-tree for every row it checks, which was a
// third of the cost of an insert.
static const char* kTableSQL =
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
    "    UNIQUE(file_id, class_name)"   // also the index of class by file
    ");"

    "CREATE TABLE IF NOT EXISTS base_class ("
    "    class_id        TEXT NOT NULL REFERENCES class(class_id) ON DELETE CASCADE,"
    "    base_class_name TEXT NOT NULL,"
    "    ordinal         INTEGER NOT NULL,"
    "    PRIMARY KEY (class_id, ordinal)"
    ") WITHOUT ROWID;"

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

    "CREATE TABLE IF NOT EXISTS param ("
    "    function_id TEXT NOT NULL REFERENCES function(function_id) ON DELETE CASCADE,"
    "    param_name  TEXT NOT NULL,"
    "    ordinal     INTEGER NOT NULL,"
    "    PRIMARY KEY (function_id, ordinal)"
    ") WITHOUT ROWID;"

    "CREATE TABLE IF NOT EXISTS field ("
    "    class_id   TEXT NOT NULL REFERENCES class(class_id) ON DELETE CASCADE,"
    "    field_name TEXT NOT NULL,"
    "    access     TEXT NOT NULL DEFAULT '+',"
    "    PRIMARY KEY (class_id, field_name)"
    ") WITHOUT ROWID;"

    "CREATE TABLE IF NOT EXISTS reading_sequence ("
    "    entity_id      TEXT PRIMARY KEY,"
    "    entity_type    TEXT NOT NULL"
    "                   CHECK(entity_type = 'file' OR entity_type = 'class' OR entity_type = 'function'),"
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
    "    kind        TEXT NOT NULL CHECK(kind = 'class' OR kind = 'interface' OR kind = 'struct' OR"
    "                                    kind = 'trait' OR kind = 'enum' OR kind = 'function' OR"
    "                                    kind = 'method' OR kind = 'field' OR kind = 'variable' OR"
    "                                    kind = 'type_alias' OR kind = 'namespace'),"
    "    name        TEXT NOT NULL,"
    "    namespace   TEXT NOT NULL DEFAULT 'value' CHECK(namespace = 'value' OR namespace = 'type'),"
    "    is_exported INTEGER NOT NULL DEFAULT 1,"
    "    signature   TEXT,"
    "    start_line  INTEGER NOT NULL,"
    "    end_line    INTEGER NOT NULL"
    ");"

    // Lexical scopes. The file scope's id is the file_id; a class or function
    // scope's id is its symbol id, which is also owner_id. A scope id starts
    // with its file_id, so (file_id, scope_id) is as unique as scope_id, and
    // as the key it is also the index of a file's scopes.
    "CREATE TABLE IF NOT EXISTS scope ("
    "    scope_id        TEXT NOT NULL,"
    "    file_id         TEXT NOT NULL REFERENCES file(file_id) ON DELETE CASCADE,"
    "    parent_scope_id TEXT,"
    "    kind            TEXT NOT NULL CHECK(kind = 'file' OR kind = 'namespace' OR kind = 'class' OR"
    "                                    kind = 'function' OR kind = 'block'),"
    "    owner_id        TEXT,"
    "    start_line      INTEGER NOT NULL,"
    "    end_line        INTEGER NOT NULL,"
    "    PRIMARY KEY (file_id, scope_id)"
    ") WITHOUT ROWID;"

    // What a name means inside one scope. target_id is the defining symbol for
    // `def`, NULL for a local with no symbol row (parameters, function locals).
    // A name bound more than once in a scope has one row per line; choosing
    // among them is the resolver's job. is_exported: as symbol.is_exported, for
    // module-level bindings -- what a wildcard import of the module sees.
    // file_id leads the key as in scope: the same rows are unique, and a
    // file's bindings are one range of it.
    "CREATE TABLE IF NOT EXISTS binding ("
    "    scope_id      TEXT NOT NULL,"
    "    local_name    TEXT NOT NULL,"
    "    namespace     TEXT NOT NULL DEFAULT 'value' CHECK(namespace = 'value' OR namespace = 'type'),"
    "    kind          TEXT NOT NULL CHECK(kind = 'def' OR kind = 'import_module' OR"
    "                                      kind = 'import_symbol' OR kind = 'import_all' OR"
    "                                      kind = 'reexport'),"
    "    file_id       TEXT NOT NULL REFERENCES file(file_id) ON DELETE CASCADE,"
    "    target_id     TEXT,"
    "    module_spec   TEXT,"
    "    imported_name TEXT,"
    "    line          INTEGER NOT NULL,"
    "    is_exported   INTEGER NOT NULL DEFAULT 1,"
    "    PRIMARY KEY (file_id, scope_id, namespace, local_name, line)"
    ") WITHOUT ROWID;"

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

    // An unresolved use of a name. resolved_id has no FK: it may point into a
    // file that is re-scanned later, and the resolver re-checks it then.
    // emits_link: 1 on the ref that stands for a `link` row (kLinkViewSQL) --
    // resolved to a project symbol of the kind its link type takes -- and only
    // on the first such ref (lowest ref_id) of each link, so the view needs no
    // DISTINCT. Written by the resolver; idx_ref_link indexes these refs.
    // (source_id, kind, name_path, line) is unique -- the same callee twice on
    // one line is one ref -- which DbInserter and the resolver keep to rather
    // than an index: nothing looks a ref up by it.
    "CREATE TABLE IF NOT EXISTS ref ("
    "    ref_id      INTEGER PRIMARY KEY,"
    "    file_id     TEXT NOT NULL REFERENCES file(file_id) ON DELETE CASCADE,"
    "    source_id   TEXT NOT NULL,"
    "    scope_id    TEXT NOT NULL,"
    "    kind        TEXT NOT NULL CHECK(kind = 'CALLS' OR kind = 'INHERITS' OR kind = 'IMPLEMENTS' OR"
    "                                    kind = 'DECORATES'),"
    "    subkind     TEXT,"
    "    namespace   TEXT NOT NULL DEFAULT 'value' CHECK(namespace = 'value' OR namespace = 'type'),"
    "    name_path   TEXT NOT NULL,"
    "    raw_text    TEXT NOT NULL,"
    "    line        INTEGER NOT NULL,"
    "    resolved_id TEXT,"
    "    status      TEXT NOT NULL DEFAULT 'pending'"
    "                CHECK(status = 'pending' OR status = 'resolved' OR status = 'builtin' OR"
    "                      status = 'external' OR status = 'unresolved'),"
    "    confidence  TEXT CHECK(confidence = 'exact' OR confidence = 'heuristic'),"
    "    resolver    TEXT,"
    "    emits_link  INTEGER NOT NULL DEFAULT 0"
    ");"

    // What each ref's answer was decided on (Resolver.h), so that an update
    // re-resolves only the refs whose facts it changed. fact.key is the
    // resolver's: a letter and the fact's parts. A ref's facts of its own file
    // are left out; its ref_dep rows go with it.
    "CREATE TABLE IF NOT EXISTS fact ("
    "    fact_id INTEGER PRIMARY KEY,"
    "    key     TEXT NOT NULL UNIQUE"
    ");"
    // An E fact (an answer several refs share) stands for the facts in its
    // fact_edge rows, which may be E facts in turn.
    "CREATE TABLE IF NOT EXISTS fact_edge ("
    "    parent_id INTEGER NOT NULL,"
    "    child_id  INTEGER NOT NULL,"
    "    PRIMARY KEY (parent_id, child_id)"
    ") WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS ref_dep ("
    "    fact_id INTEGER NOT NULL,"
    "    ref_id  INTEGER NOT NULL REFERENCES ref(ref_id) ON DELETE CASCADE,"
    "    PRIMARY KEY (fact_id, ref_id)"
    ") WITHOUT ROWID;";

// Every index that is not a table's own key: what reads each, and the
// cascades of a file's delete (DbUpdater), which look a file's rows up by
// file_id. Created with the tables for any database but a scan's, which
// builds them after the bulk insert (createIndexes): sorting the rows once
// is much cheaper than inserting each into every index, and leaves the
// indexes packed.
static const char* kIndexSQL =
    "CREATE INDEX IF NOT EXISTS idx_function_file ON function(file_id) WHERE file_id IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_function_class ON function(class_id) WHERE class_id IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_rs_file      ON reading_sequence(file_id);"
    "CREATE INDEX IF NOT EXISTS idx_rs_file_rank ON reading_sequence(file_rank);"
    "CREATE INDEX IF NOT EXISTS idx_symbol_file  ON symbol(file_id);"
    // DbUpdater::findImportingFiles
    "CREATE INDEX IF NOT EXISTS idx_import_resolution_target ON import_resolution(resolved_file_id);"
    // A file's refs: the resolver's per-file reads, and the cascade.
    "CREATE INDEX IF NOT EXISTS idx_ref_file     ON ref(file_id);"
    // The INHERITS refs of a class (addClassBases) and into a file's classes
    // (Resolver::beginChange), the only lookups by source or target.
    "CREATE INDEX IF NOT EXISTS idx_ref_inherits_source ON ref(source_id) WHERE kind = 'INHERITS';"
    "CREATE INDEX IF NOT EXISTS idx_ref_inherits_target ON ref(resolved_id) WHERE kind = 'INHERITS';"
    "CREATE INDEX IF NOT EXISTS idx_ref_link     ON ref(kind, ref_id, source_id, resolved_id)"
    "    WHERE emits_link = 1;"
    "CREATE INDEX IF NOT EXISTS idx_fact_edge_child ON fact_edge(child_id);"
    "CREATE INDEX IF NOT EXISTS idx_ref_dep_ref ON ref_dep(ref_id);";

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

namespace {

int execReporting(sqlite3* db, const char* who, const std::string& sql)
{
    char* errMsg = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "%s: %s\n", who, errMsg ? errMsg : sqlite3_errstr(rc));
        sqlite3_free(errMsg);
    }
    return rc;
}

}  // namespace

int createSchema(sqlite3* db, bool indexes)
{
    return execReporting(db, "createSchema",
                         std::string(kTableSQL) + (indexes ? kIndexSQL : "") + kLinkViewSQL +
                         "PRAGMA user_version = " + std::to_string(kDbVersion) + ";");
}

int createIndexes(sqlite3* db)
{
    return execReporting(db, "createIndexes", kIndexSQL);
}

int dropIndexes(sqlite3* db)
{
    // The indexes kIndexSQL creates: every one with SQL of its own, which a
    // key's index (sqlite_autoindex_*) has not.
    std::string sql;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type = 'index' AND sql IS NOT NULL;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string quoted;
        for (char c : name) quoted += c == '"' ? std::string("\"\"") : std::string(1, c);
        sql += "DROP INDEX \"" + quoted + "\";";
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return rc;
    return execReporting(db, "dropIndexes", sql);
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

namespace {

// The file a path names once every symlink on it is followed, a dangling
// one included (weakly_canonical stops at a link whose target is missing).
std::filesystem::path linkTarget(const std::filesystem::path& path)
{
    namespace fs = std::filesystem;
    fs::path p = path;
    std::error_code ec;
    for (int hops = 0; hops < 40 && fs::is_symlink(fs::symlink_status(p, ec)); ++hops) {
        const fs::path to = fs::read_symlink(p, ec);
        if (ec) break;
        p = to.is_absolute() ? to : p.parent_path() / to;
    }
    const fs::path resolved = fs::weakly_canonical(p, ec);
    return ec ? p : resolved;
}

// SQLITE_OK if files can be created and removed in the directory `file` is
// in -- SQLite needs that for the database's -wal and -shm, scan to replace
// one of another version -- else why not.
int checkDirFor(const std::filesystem::path& file)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = file.has_parent_path() ? file.parent_path() : fs::path(".");
    if (!fs::is_directory(dir, ec)) return SQLITE_CANTOPEN;
#ifndef _WIN32
    if (access(dir.c_str(), W_OK | X_OK) != 0) return SQLITE_READONLY;
#endif
    return SQLITE_OK;
}

}  // namespace

int checkScanTarget(const char* dbPath)
{
    namespace fs = std::filesystem;
    if (!dbPath) return SQLITE_MISUSE;
    DbKind kind;
    int rc = classifyPath(dbPath, &kind);
    if (rc != SQLITE_OK) return rc;
    if (kind == DbKind::NotTelescode) return kNotADatabase;
    if (std::string(dbPath) == ":memory:") return SQLITE_OK;

    // Nothing is created, opened or removed to find out: the directories
    // and the file are only looked at. The path's own directory holds what
    // openDbForScan removes (a symlink, not its target); the target's holds
    // the database and its -wal and -shm.
    const fs::path path = dbPath;
    const fs::path target = linkTarget(path);
    if ((rc = checkDirFor(path)) != SQLITE_OK) return rc;
    if ((rc = checkDirFor(target)) != SQLITE_OK) return rc;
    std::error_code ec;
    const fs::file_status st = fs::status(target, ec);
    if (fs::exists(st)) {
        if (!fs::is_regular_file(st)) return SQLITE_CANTOPEN;
#ifndef _WIN32
        // One of another version is removed rather than written, but a
        // read-only file is one the user means to keep.
        if (access(target.c_str(), R_OK | W_OK) != 0) return SQLITE_READONLY;
#endif
    }
    return SQLITE_OK;
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
        // replay the old database's pages into the new one. Through a
        // symlink, the file it names goes (SQLite names the -wal and -shm
        // after that file too), and the new database is made there: the
        // link is the user's.
        const std::string target = linkTarget(dbPath).string();
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::error_code ec;
            std::filesystem::remove(target + suffix, ec);
            if (ec) {
                std::fprintf(stderr, "scan: cannot remove '%s%s': %s\n",
                             target.c_str(), suffix, ec.message().c_str());
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
