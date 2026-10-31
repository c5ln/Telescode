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
    , stmtField_(nullptr)
    , stmtSymbol_(nullptr)
    , stmtScope_(nullptr)
    , stmtBinding_(nullptr)
    , stmtRef_(nullptr)
    , stmtImport_(nullptr)
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
        "INSERT OR REPLACE INTO file(file_id, file_name, language, raw_loc, logical_loc, is_generated, max_cyclomatic_complexity, avg_cyclomatic_complexity, max_block_depth, avg_block_depth, max_function_loc, avg_function_loc, module_name) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);",
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
        "INSERT OR REPLACE INTO function(function_id, file_id, class_id, function_name, nesting_depth, is_async, cyclomatic_complexity, max_block_depth, loc, start_line, end_line) VALUES(?,?,?,?,?,?,?,?,?,?,?);",
        -1, &stmtFunction_, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO param(function_id, param_name, ordinal) VALUES(?,?,?);",
        -1, &stmtParam_, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO field(class_id, field_name, access) VALUES(?,?,?);",
        -1, &stmtField_, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO symbol(symbol_id, file_id, parent_id, kind, name, namespace, is_exported, start_line, end_line) VALUES(?,?,?,?,?,?,?,?,?);",
        -1, &stmtSymbol_, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO scope(scope_id, file_id, parent_scope_id, kind, owner_id, start_line, end_line) VALUES(?,?,?,?,?,?,?);",
        -1, &stmtScope_, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO binding(scope_id, local_name, namespace, kind, file_id, target_id, module_spec, imported_name, line, is_exported) VALUES(?,?,?,?,?,?,?,?,?,?);",
        -1, &stmtBinding_, nullptr);
    if (rc != SQLITE_OK) return rc;

    // OR IGNORE: the same callee twice on one line is one ref (UNIQUE key).
    rc = sqlite3_prepare_v2(db_,
        "INSERT OR IGNORE INTO ref(file_id, source_id, scope_id, kind, namespace, name_path, raw_text, line) VALUES(?,?,?,?,?,?,?,?);",
        -1, &stmtRef_, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_prepare_v2(db_,
        "INSERT OR IGNORE INTO import_resolution(file_id, module_spec) VALUES(?,?);",
        -1, &stmtImport_, nullptr);
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
    sqlite3_finalize(stmtField_);     stmtField_     = nullptr;
    sqlite3_finalize(stmtSymbol_);    stmtSymbol_    = nullptr;
    sqlite3_finalize(stmtScope_);     stmtScope_     = nullptr;
    sqlite3_finalize(stmtBinding_);   stmtBinding_   = nullptr;
    sqlite3_finalize(stmtRef_);       stmtRef_       = nullptr;
    sqlite3_finalize(stmtImport_);    stmtImport_    = nullptr;
}

// ── Insert helpers ────────────────────────────────────────────────────────────

// Empty string -> SQL NULL, for optional TEXT columns.
static void bindTextOrNull(sqlite3_stmt* stmt, int idx, const std::string& s)
{
    if (s.empty())
        sqlite3_bind_null(stmt, idx);
    else
        sqlite3_bind_text(stmt, idx, s.c_str(), -1, SQLITE_STATIC);
}

int DbInserter::insertFile(const FileEntity& e)
{
    sqlite3_reset(stmtFile_);
    sqlite3_bind_text  (stmtFile_, 1, e.file_id.c_str(),   -1, SQLITE_STATIC);
    sqlite3_bind_text  (stmtFile_, 2, e.file_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text  (stmtFile_, 3, e.language.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_int   (stmtFile_, 4,  e.raw_loc);
    sqlite3_bind_int   (stmtFile_, 5,  e.logical_loc);
    sqlite3_bind_int   (stmtFile_, 6,  e.is_generated);
    sqlite3_bind_int   (stmtFile_, 7,  e.max_cyclomatic_complexity);
    sqlite3_bind_double(stmtFile_, 8,  e.avg_cyclomatic_complexity);
    sqlite3_bind_int   (stmtFile_, 9,  e.max_block_depth);
    sqlite3_bind_double(stmtFile_, 10, e.avg_block_depth);
    sqlite3_bind_int   (stmtFile_, 11, e.max_function_loc);
    sqlite3_bind_double(stmtFile_, 12, e.avg_function_loc);
    bindTextOrNull     (stmtFile_, 13, e.module_name);
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
    sqlite3_bind_int (stmtFunction_, 7,  e.cyclomatic_complexity);
    sqlite3_bind_int (stmtFunction_, 8,  e.max_block_depth);
    sqlite3_bind_int (stmtFunction_, 9,  e.loc);
    sqlite3_bind_int (stmtFunction_, 10, e.start_line);
    sqlite3_bind_int (stmtFunction_, 11, e.end_line);
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

int DbInserter::insertSymbol(const SymbolEntity& e)
{
    sqlite3_reset(stmtSymbol_);
    sqlite3_bind_text(stmtSymbol_, 1, e.symbol_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtSymbol_, 2, e.file_id.c_str(),   -1, SQLITE_STATIC);
    bindTextOrNull   (stmtSymbol_, 3, e.parent_id);
    sqlite3_bind_text(stmtSymbol_, 4, e.kind.c_str(),      -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtSymbol_, 5, e.name.c_str(),      -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtSymbol_, 6, e.ns.c_str(),        -1, SQLITE_STATIC);
    sqlite3_bind_int (stmtSymbol_, 7, e.is_exported);
    sqlite3_bind_int (stmtSymbol_, 8, e.start_line);
    sqlite3_bind_int (stmtSymbol_, 9, e.end_line);
    int rc = sqlite3_step(stmtSymbol_);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int DbInserter::insertScope(const ScopeEntity& e)
{
    sqlite3_reset(stmtScope_);
    sqlite3_bind_text(stmtScope_, 1, e.scope_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtScope_, 2, e.file_id.c_str(),  -1, SQLITE_STATIC);
    bindTextOrNull   (stmtScope_, 3, e.parent_scope_id);
    sqlite3_bind_text(stmtScope_, 4, e.kind.c_str(),     -1, SQLITE_STATIC);
    bindTextOrNull   (stmtScope_, 5, e.owner_id);
    sqlite3_bind_int (stmtScope_, 6, e.start_line);
    sqlite3_bind_int (stmtScope_, 7, e.end_line);
    int rc = sqlite3_step(stmtScope_);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int DbInserter::insertBinding(const BindingEntity& e)
{
    sqlite3_reset(stmtBinding_);
    sqlite3_bind_text(stmtBinding_, 1, e.scope_id.c_str(),   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtBinding_, 2, e.local_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtBinding_, 3, e.ns.c_str(),         -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtBinding_, 4, e.kind.c_str(),       -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtBinding_, 5, e.file_id.c_str(),    -1, SQLITE_STATIC);
    bindTextOrNull   (stmtBinding_, 6, e.target_id);
    bindTextOrNull   (stmtBinding_, 7, e.module_spec);
    bindTextOrNull   (stmtBinding_, 8, e.imported_name);
    sqlite3_bind_int (stmtBinding_, 9, e.line);
    sqlite3_bind_int (stmtBinding_, 10, e.is_exported);
    int rc = sqlite3_step(stmtBinding_);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int DbInserter::insertRef(const RefEntity& e)
{
    sqlite3_reset(stmtRef_);
    sqlite3_bind_text(stmtRef_, 1, e.file_id.c_str(),   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtRef_, 2, e.source_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtRef_, 3, e.scope_id.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtRef_, 4, e.kind.c_str(),      -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtRef_, 5, e.ns.c_str(),        -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtRef_, 6, e.name_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtRef_, 7, e.raw_text.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_int (stmtRef_, 8, e.line);
    int rc = sqlite3_step(stmtRef_);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

// A 'pending' import_resolution row; the resolver says what the spec names.
int DbInserter::insertImportSpec(const std::string& fileId, const std::string& spec)
{
    sqlite3_reset(stmtImport_);
    sqlite3_bind_text(stmtImport_, 1, fileId.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmtImport_, 2, spec.c_str(),   -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmtImport_);
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

        for (const FieldEntity& f : r.fields) {
            rc = inserter.insertField(f);
            if (rc != SQLITE_OK) return rc;
        }

        for (const SymbolEntity& s : r.symbols) {
            rc = inserter.insertSymbol(s);
            if (rc != SQLITE_OK) return rc;
        }

        for (const ScopeEntity& s : r.scopes) {
            rc = inserter.insertScope(s);
            if (rc != SQLITE_OK) return rc;
        }

        for (const BindingEntity& b : r.bindings) {
            rc = inserter.insertBinding(b);
            if (rc != SQLITE_OK) return rc;
        }

        for (const RefEntity& ref : r.refs) {
            rc = inserter.insertRef(ref);
            if (rc != SQLITE_OK) return rc;
        }

        for (const std::string& spec : r.import_specs) {
            rc = inserter.insertImportSpec(r.file.file_id, spec);
            if (rc != SQLITE_OK) return rc;
        }
    }

    return SQLITE_OK;
}

// ── insertAll ─────────────────────────────────────────────────────────────────

int DbInserter::insertAll(const char* dbPath,
                          const std::vector<ParseResult>& results,
                          Resolver::Stats* stats,
                          const LanguageRegistry& languages)
{
    sqlite3* db = nullptr;
    bool empty = false;
    int rc = openDbForScan(dbPath, &db, &empty);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) { sqlite3_close(db); return rc; }

    // In this transaction: a new database gets its version only together
    // with everything below.
    if (empty) {
        rc = createSchema(db);
        if (rc != SQLITE_OK) goto rollback;
    }

    rc = sqlite3_exec(db, "DELETE FROM file;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) goto rollback;

    rc = insertResults(db, results);
    if (rc != SQLITE_OK) goto rollback;

    // Names across files can only be resolved once every file is in.
    rc = Resolver::resolveAll(db, stats, languages);
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
