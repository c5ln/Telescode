#pragma once

#include "parser/ParseResult.h"
#include <string>
#include <vector>

struct sqlite3;

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

    static std::vector<std::string> findImportingFiles(const char* dbPath,
                                                        const std::string& fileId);

private:
    static int deleteFileEntities(sqlite3* db, const std::string& fileId);
};
