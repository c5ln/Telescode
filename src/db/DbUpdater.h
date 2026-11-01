#pragma once

#include "parser/ParseResult.h"
#include <string>
#include <vector>

struct sqlite3;

// Every operation returns kWrongDbVersion (db.h), and leaves the file
// untouched, on a database of another version. Each change is one
// transaction, and re-resolves only the refs it can have changed
// (Resolver::resolveChanged): the database is then what a scan of the same
// files gives.
class DbUpdater {
public:
    static int updateFile(const char* dbPath, const ParseResult& result);
    static int updateFiles(const char* dbPath,
                           const std::vector<ParseResult>& results);

    static int deleteFile(const char* dbPath, const std::string& fileId);
    static int deleteFiles(const char* dbPath,
                           const std::vector<std::string>& fileIds);

    static int renameFile(const char* dbPath,
                          const std::string& oldFileId,
                          const ParseResult& newResult);

    // The files whose imports resolve to fileId, sorted: by a module spec
    // that names it, or by `from pkg import mod` binding a name to it. The
    // file itself is left out. Read-only.
    static int findImportingFiles(const char* dbPath, const std::string& fileId,
                                  std::vector<std::string>& importers);

private:
    static int deleteFileEntities(sqlite3* db, const std::string& fileId);
};
