#pragma once

#include "parser/ParseResult.h"
#include "resolve/Resolver.h"
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

class DbInserter {
public:
    // `scan`: the database at dbPath now holds exactly `results`, resolved
    // (Resolver::resolveAll). One transaction for all of it, with the schema
    // too when the file is new: if it fails or is cut off, the database is
    // left as it was before, or -- if it was new, or of another version and so
    // deleted (openDbForScan) -- has nothing in it, not even a version.
    static int insertAll(const char* dbPath,
                         const std::vector<ParseResult>& results,
                         Resolver::Stats* stats = nullptr,
                         const LanguageRegistry& languages = LanguageRegistry::builtin());

    // Insert results into an already-open db without managing the transaction.
    // Caller is responsible for BEGIN/COMMIT/ROLLBACK.
    static int insertResults(sqlite3* db,
                             const std::vector<ParseResult>& results);

private:
    explicit DbInserter(sqlite3* db);
    ~DbInserter();
    DbInserter(const DbInserter&) = delete;
    DbInserter& operator=(const DbInserter&) = delete;

    int prepareStatements();
    void finalizeStatements();

    int insertFile(const FileEntity& e);
    int insertClass(const ClassEntity& e);
    int insertBaseClass(const BaseClassEntity& e);
    int insertFunction(const FunctionEntity& e);
    int insertParam(const ParamEntity& e);
    int insertField(const FieldEntity& e);
    int insertSymbol(const SymbolEntity& e);
    int insertScope(const ScopeEntity& e);
    int insertBinding(const BindingEntity& e);
    int insertRef(const RefEntity& e);
    int insertImportSpec(const std::string& fileId, const std::string& spec);

    sqlite3*      db_;
    sqlite3_stmt* stmtFile_;
    sqlite3_stmt* stmtClass_;
    sqlite3_stmt* stmtBaseClass_;
    sqlite3_stmt* stmtFunction_;
    sqlite3_stmt* stmtParam_;
    sqlite3_stmt* stmtField_;
    sqlite3_stmt* stmtSymbol_;
    sqlite3_stmt* stmtScope_;
    sqlite3_stmt* stmtBinding_;
    sqlite3_stmt* stmtRef_;
    sqlite3_stmt* stmtImport_;
};
