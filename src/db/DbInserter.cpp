#include "DbInserter.h"
#include "db.h"
#include <sqlite3.h>

// ── Constructor / Destructor ──────────────────────────────────────────────────

DbInserter::DbInserter(sqlite3* db)
    : db_(db)
    , stmtFile_(nullptr)
    , stmtClass_(nullptr)
    , stmtBaseClass_(nullptr)
    , stmtFunction_(nullptr)
    , stmtParam_(nullptr)
    , stmtLink_(nullptr)
    , stmtField_(nullptr)
{}

DbInserter::~DbInserter()
{
    finalizeStatements();
}

// ── Statement lifecycle ───────────────────────────────────────────────────────

int DbInserter::prepareStatements()
{
    int rc;

    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO file(file_id, file_name, language, loc) VALUES(?,?,?,?);",
        -1, &stmtFile_, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO class(class_id, file_id, class_name, start_line, end_line) VALUES(?,?,?,?,?);",
        -1, &stmtClass_, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO base_class(class_id, base_class_name, ordinal) VALUES(?,?,?);",
        -1, &stmtBaseClass_, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO function(function_id, file_id, class_id, function_name, nesting_depth, is_async, cyclomatic_complexity, start_line, end_line) VALUES(?,?,?,?,?,?,?,?,?);",
        -1, &stmtFunction_, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO param(function_id, param_name, ordinal) VALUES(?,?,?);",
        -1, &stmtParam_, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO link(source_id, target_id, link_type) VALUES(?,?,?);",
        -1, &stmtLink_, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO field(class_id, field_name, access) VALUES(?,?,?);",
        -1, &stmtField_, nullptr);
    if (rc != SQLITE_OK) return rc;

    return SQLITE_OK;
}

void DbInserter::finalizeStatements()
{
    sqlite3_finalize(stmtFile_);      stmtFile_      = nullptr;
    sqlite3_finalize(stmtClass_);     stmtClass_     = nullptr;
    sqlite3_finalize(stmtBaseClass_); stmtBaseClass_ = nullptr;
    sqlite3_finalize(stmtFunction_);  stmtFunction_  = nullptr;
    sqlite3_finalize(stmtParam_);     stmtParam_     = nullptr;
    sqlite3_finalize(stmtLink_);      stmtLink_      = nullptr;
    sqlite3_finalize(stmtField_);     stmtField_     = nullptr;
}

// ── Insert helpers ────────────────────────────────────────────────────────────

int DbInserter::insertFile(const FileEntity& e)
{
    sqlite3_reset(stmtFile_);
    sqlite3_bind_text(stmtFile_, 1, e.file_id.c_str(),   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtFile_, 2, e.file_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtFile_, 3, e.language.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_int (stmtFile_, 4, e.loc);
    int rc = sqlite3_step(stmtFile_);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int DbInserter::insertClass(const ClassEntity& e)
{
    sqlite3_reset(stmtClass_);
    sqlite3_bind_text(stmtClass_, 1, e.class_id.c_str(),   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtClass_, 2, e.file_id.c_str(),    -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtClass_, 3, e.class_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int (stmtClass_, 4, e.start_line);
    sqlite3_bind_int (stmtClass_, 5, e.end_line);
    int rc = sqlite3_step(stmtClass_);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int DbInserter::insertBaseClass(const BaseClassEntity& e)
{
    sqlite3_reset(stmtBaseClass_);
    sqlite3_bind_text(stmtBaseClass_, 1, e.class_id.c_str(),        -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtBaseClass_, 2, e.base_class_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int (stmtBaseClass_, 3, e.ordinal);
    int rc = sqlite3_step(stmtBaseClass_);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int DbInserter::insertFunction(const FunctionEntity& e)
{
    sqlite3_reset(stmtFunction_);
    sqlite3_bind_text(stmtFunction_, 1, e.function_id.c_str(),   -1, SQLITE_STATIC);
    if (e.file_id.empty())
        sqlite3_bind_null(stmtFunction_, 2);
    else
        sqlite3_bind_text(stmtFunction_, 2, e.file_id.c_str(),   -1, SQLITE_STATIC);
    if (e.class_id.empty())
        sqlite3_bind_null(stmtFunction_, 3);
    else
        sqlite3_bind_text(stmtFunction_, 3, e.class_id.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtFunction_, 4, e.function_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int (stmtFunction_, 5, e.nesting_depth);
    sqlite3_bind_int (stmtFunction_, 6, e.is_async);
    sqlite3_bind_int (stmtFunction_, 7, e.cyclomatic_complexity);
    sqlite3_bind_int (stmtFunction_, 8, e.start_line);
    sqlite3_bind_int (stmtFunction_, 9, e.end_line);
    int rc = sqlite3_step(stmtFunction_);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int DbInserter::insertParam(const ParamEntity& e)
{
    sqlite3_reset(stmtParam_);
    sqlite3_bind_text(stmtParam_, 1, e.function_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtParam_, 2, e.param_name.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_int (stmtParam_, 3, e.ordinal);
    int rc = sqlite3_step(stmtParam_);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int DbInserter::insertLink(const LinkEntity& e)
{
    sqlite3_reset(stmtLink_);
    sqlite3_bind_text(stmtLink_, 1, e.source_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtLink_, 2, e.target_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtLink_, 3, e.link_type.c_str(), -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmtLink_);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int DbInserter::insertField(const FieldEntity& e)
{
    const char access_str[2] = { e.access, '\0' };
    sqlite3_reset(stmtField_);
    sqlite3_bind_text(stmtField_, 1, e.class_id.c_str(),   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtField_, 2, e.field_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtField_, 3, access_str,            -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmtField_);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

// ── insertResults ─────────────────────────────────────────────────────────────

int DbInserter::insertResults(sqlite3* db,
                               const std::vector<ParseResult>& results)
{
    DbInserter inserter(db);
    int rc = inserter.prepareStatements();
    if (rc != SQLITE_OK) return rc;
    // ~DbInserter() finalizes statements on any return path

    for (const ParseResult& r : results) {
        rc = inserter.insertFile(r.file);
        if (rc != SQLITE_OK) return rc;

        for (const ClassEntity& c : r.classes) {
            rc = inserter.insertClass(c);
            if (rc != SQLITE_OK) return rc;
        }

        for (const BaseClassEntity& b : r.base_classes) {
            rc = inserter.insertBaseClass(b);
            if (rc != SQLITE_OK) return rc;
        }

        for (const FunctionEntity& f : r.functions) {
            rc = inserter.insertFunction(f);
            if (rc != SQLITE_OK) return rc;
        }

        for (const ParamEntity& p : r.params) {
            rc = inserter.insertParam(p);
            if (rc != SQLITE_OK) return rc;
        }

        for (const LinkEntity& l : r.links) {
            rc = inserter.insertLink(l);
            if (rc != SQLITE_OK) return rc;
        }

        for (const FieldEntity& f : r.fields) {
            rc = inserter.insertField(f);
            if (rc != SQLITE_OK) return rc;
        }
    }

    return SQLITE_OK;
}

// ── insertAll ─────────────────────────────────────────────────────────────────

int DbInserter::insertAll(const char* dbPath,
                          const std::vector<ParseResult>& results)
{
    sqlite3* db = nullptr;
    int rc = initDb(dbPath, &db);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) { sqlite3_close(db); return rc; }

    rc = sqlite3_exec(db, "DELETE FROM file;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) goto rollback;

    rc = insertResults(db, results);
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
