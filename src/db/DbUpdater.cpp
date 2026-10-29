#include "DbUpdater.h"
#include "DbInserter.h"
#include "db.h"
#include "resolve/Resolver.h"
#include <sqlite3.h>
#include <cstring>

// ── deleteFileEntities ────────────────────────────────────────────────────────
// Only the IMPORTS links this deletes matter now: every operation below ends
// by re-running the resolver over the whole database, which rewrites all
// CALLS / INHERITS / DECORATES links -- including the ones into this file that
// the LIKE below removes and that a per-file re-parse could not restore.

int DbUpdater::deleteFileEntities(sqlite3* db, const std::string& fileId)
{
    const std::string like = fileId + "::%";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "DELETE FROM link"
        " WHERE source_id = ? OR source_id LIKE ?"
        "    OR target_id = ? OR target_id LIKE ?;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return rc;

    sqlite3_bind_text(stmt, 1, fileId.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, like.c_str(),   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, fileId.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, like.c_str(),   -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return rc;

    stmt = nullptr;
    rc = sqlite3_prepare_v2(db,
        "DELETE FROM file WHERE file_id = ?;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return rc;

    sqlite3_bind_text(stmt, 1, fileId.c_str(), -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

// ── checkScanned ──────────────────────────────────────────────────────────────
// A database scanned by an older Telescode lacks rows the resolver needs (no
// scopes or bindings at all before Stage 2, no subscripted-base refs or
// binding.is_exported before Stage 3), so the resolver run at the end of each
// operation would drop links. Its files carry no current scan stamp. Refuse
// before opening it with initDb, which would already migrate it: the file is
// left exactly as it was.

static int checkScanned(const char* dbPath)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return SQLITE_OK;   // no database yet: nothing to protect
    }
    const bool stale = Resolver::needsRescan(db);
    sqlite3_close(db);
    return stale ? Resolver::kNeedsRescan : SQLITE_OK;
}

// ── updateFiles ───────────────────────────────────────────────────────────────

int DbUpdater::updateFiles(const char* dbPath,
                            const std::vector<ParseResult>& results)
{
    int rc = checkScanned(dbPath);
    if (rc != SQLITE_OK) return rc;

    sqlite3* db = nullptr;
    rc = initDb(dbPath, &db);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) { sqlite3_close(db); return rc; }

    // checkScanned passed: every file left is this parser's (or there are none).
    rc = stampScanFormat(db);
    if (rc != SQLITE_OK) goto rollback;

    for (const ParseResult& r : results) {
        rc = deleteFileEntities(db, r.file.file_id);
        if (rc != SQLITE_OK) goto rollback;
    }

    rc = DbInserter::insertResults(db, results);
    if (rc != SQLITE_OK) goto rollback;

    // Full re-resolution after every change, until it is made incremental.
    rc = Resolver::resolveAll(db);
    if (rc != SQLITE_OK) goto rollback;

    rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) goto rollback;

    sqlite3_close(db);
    return SQLITE_OK;

rollback:
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return rc;
}

int DbUpdater::updateFile(const char* dbPath, const ParseResult& result)
{
    return updateFiles(dbPath, {result});
}

// ── deleteFiles ───────────────────────────────────────────────────────────────

int DbUpdater::deleteFiles(const char* dbPath,
                            const std::vector<std::string>& fileIds)
{
    int rc = checkScanned(dbPath);
    if (rc != SQLITE_OK) return rc;

    sqlite3* db = nullptr;
    rc = initDb(dbPath, &db);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) { sqlite3_close(db); return rc; }

    // checkScanned passed: every file left is this parser's (or there are none).
    rc = stampScanFormat(db);
    if (rc != SQLITE_OK) goto rollback;

    for (const std::string& id : fileIds) {
        rc = deleteFileEntities(db, id);
        if (rc != SQLITE_OK) goto rollback;
    }

    // Full re-resolution after every change, until it is made incremental.
    rc = Resolver::resolveAll(db);
    if (rc != SQLITE_OK) goto rollback;

    rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) goto rollback;

    sqlite3_close(db);
    return SQLITE_OK;

rollback:
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return rc;
}

int DbUpdater::deleteFile(const char* dbPath, const std::string& fileId)
{
    return deleteFiles(dbPath, {fileId});
}

// ── renameFile ────────────────────────────────────────────────────────────────

int DbUpdater::renameFile(const char* dbPath,
                           const std::string& oldFileId,
                           const ParseResult& newResult)
{
    int rc = checkScanned(dbPath);
    if (rc != SQLITE_OK) return rc;

    sqlite3* db = nullptr;
    rc = initDb(dbPath, &db);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) { sqlite3_close(db); return rc; }

    // checkScanned passed: every file left is this parser's (or there are none).
    rc = stampScanFormat(db);
    if (rc != SQLITE_OK) goto rollback;

    rc = deleteFileEntities(db, oldFileId);
    if (rc != SQLITE_OK) goto rollback;

    rc = DbInserter::insertResults(db, {newResult});
    if (rc != SQLITE_OK) goto rollback;

    // Full re-resolution after every change, until it is made incremental.
    rc = Resolver::resolveAll(db);
    if (rc != SQLITE_OK) goto rollback;

    rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) goto rollback;

    sqlite3_close(db);
    return SQLITE_OK;

rollback:
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return rc;
}

// ── findImportingFiles ────────────────────────────────────────────────────────

// Convert "src/utils/foo.py" -> "src.utils.foo" to match how parsers store
// import targets (module name, not file path).
static std::string fileIdToModuleName(const std::string& fileId)
{
    std::string s = fileId;
    // Strip known source-file extensions
    for (const char* ext : {".py", ".ts", ".tsx", ".js", ".jsx"}) {
        size_t extLen = std::strlen(ext);
        if (s.size() > extLen &&
            s.compare(s.size() - extLen, extLen, ext) == 0) {
            s.erase(s.size() - extLen);
            break;
        }
    }
    // Replace path separators with dots: src/utils/foo -> src.utils.foo
    for (char& c : s) {
        if (c == '/' || c == '\\') c = '.';
    }
    return s;
}

std::vector<std::string> DbUpdater::findImportingFiles(const char* dbPath,
                                                         const std::string& fileId)
{
    sqlite3* db = nullptr;
    if (initDb(dbPath, &db) != SQLITE_OK) return {};

    // Search by both the raw file_id and the derived module name so the
    // function works regardless of what the caller passes.
    const std::string moduleName = fileIdToModuleName(fileId);
    const std::string fileLike   = fileId     + "::%";
    const std::string moduleLike = moduleName + "::%";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "SELECT DISTINCT source_id FROM link"
        " WHERE link_type = 'IMPORTS'"
        "   AND (target_id = ? OR target_id LIKE ?"   // raw file_id form
        "     OR target_id = ? OR target_id LIKE ?);", // module name form
        -1, &stmt, nullptr);

    std::vector<std::string> result;
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, fileId.c_str(),      -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, fileLike.c_str(),    -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, moduleName.c_str(),  -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, moduleLike.c_str(),  -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* src = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 0));
            if (src) result.emplace_back(src);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}
