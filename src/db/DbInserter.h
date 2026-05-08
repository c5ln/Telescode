#pragma once

#include "parser/ParseResult.h"
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

class DbInserter {
public:
    static int insertAll(const char* dbPath,
                         const std::vector<ParseResult>& results);

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
    int insertLink(const LinkEntity& e);

    sqlite3*      db_;
    sqlite3_stmt* stmtFile_;
    sqlite3_stmt* stmtClass_;
    sqlite3_stmt* stmtBaseClass_;
    sqlite3_stmt* stmtFunction_;
    sqlite3_stmt* stmtParam_;
    sqlite3_stmt* stmtLink_;
};
