#include "DbUpdater.h"
#include "DbInserter.h"
#include "db.h"
#include "resolve/Resolver.h"
#include <sqlite3.h>

#include <algorithm>

// ── deleteFileEntities ────────────────────────────────────────────────────────
// Every row the file contributed goes with its file row, by cascade -- the
// refs and import_resolution rows `link` is derived from included, and the
// refs' ref_dep rows. Links into the file are re-derived too: every operation
// below ends with Resolver::resolveChanged, which re-resolves every ref that
// read something of a file the operation touched.

int DbUpdater::deleteFileEntities(sqlite3* db, const std::string& fileId)
{
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "DELETE FROM file WHERE file_id = ?;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return rc;

    sqlite3_bind_text(stmt, 1, fileId.c_str(), -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

// ── updateFiles ───────────────────────────────────────────────────────────────

int DbUpdater::updateFiles(const char* dbPath,
                            const std::vector<ParseResult>& results)
{
    Resolver::Stats stats;
    Resolver::Change change;
    sqlite3* db = nullptr;
    int rc = initDb(dbPath, &db);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) { sqlite3_close(db); return rc; }

    {
        std::vector<std::string> files;
        for (const ParseResult& r : results) files.push_back(r.file.file_id);
        rc = Resolver::beginChange(db, files, change);
        if (rc != SQLITE_OK) goto rollback;
    }

    for (const ParseResult& r : results) {
        rc = deleteFileEntities(db, r.file.file_id);
        if (rc != SQLITE_OK) goto rollback;
    }

    rc = DbInserter::insertResults(db, results);
    if (rc != SQLITE_OK) goto rollback;

    rc = Resolver::resolveChanged(db, change, &stats);
    if (rc != SQLITE_OK) goto rollback;
    Resolver::warnIfCapped(stats, "update");

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
    Resolver::Stats stats;
    Resolver::Change change;
    sqlite3* db = nullptr;
    int rc = initDb(dbPath, &db);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) { sqlite3_close(db); return rc; }

    rc = Resolver::beginChange(db, fileIds, change);
    if (rc != SQLITE_OK) goto rollback;

    for (const std::string& id : fileIds) {
        rc = deleteFileEntities(db, id);
        if (rc != SQLITE_OK) goto rollback;
    }

    rc = Resolver::resolveChanged(db, change, &stats);
    if (rc != SQLITE_OK) goto rollback;
    Resolver::warnIfCapped(stats, "update");

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
    Resolver::Stats stats;
    Resolver::Change change;
    sqlite3* db = nullptr;
    int rc = initDb(dbPath, &db);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) { sqlite3_close(db); return rc; }

    rc = Resolver::beginChange(db, {oldFileId, newResult.file.file_id}, change);
    if (rc != SQLITE_OK) goto rollback;

    rc = deleteFileEntities(db, oldFileId);
    if (rc != SQLITE_OK) goto rollback;

    rc = DbInserter::insertResults(db, {newResult});
    if (rc != SQLITE_OK) goto rollback;

    rc = Resolver::resolveChanged(db, change, &stats);
    if (rc != SQLITE_OK) goto rollback;
    Resolver::warnIfCapped(stats, "update");

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

int DbUpdater::findImportingFiles(const char* dbPath, const std::string& fileId,
                                  std::vector<std::string>& importers)
{
    importers.clear();
    sqlite3* db = nullptr;
    int rc = initDb(dbPath, &db);
    if (rc != SQLITE_OK) return rc;

    // Absolute or relative, the resolver has already turned each module spec
    // into the file it names. `from pkg import mod` names pkg, though: that it
    // imports pkg/mod.py too only the binding of `mod` says, which the
    // resolver looks up. A file importing itself does not dangle when it goes.
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db,
        "SELECT DISTINCT file_id FROM import_resolution"
        " WHERE resolved_file_id = ? AND status = 'resolved' AND file_id <> resolved_file_id;",
        -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, fileId.c_str(), -1, SQLITE_STATIC);
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const char* src = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (src) importers.emplace_back(src);
        }
        rc = (rc == SQLITE_DONE) ? SQLITE_OK : rc;
    }
    if (rc == SQLITE_OK) rc = Resolver::findSubmoduleImporters(db, fileId, importers);
    std::sort(importers.begin(), importers.end());
    importers.erase(std::unique(importers.begin(), importers.end()), importers.end());
    if (rc != SQLITE_OK) importers.clear();

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}
