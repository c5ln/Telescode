#include "DbInserter.h"
#include "parser/PythonParser.h"
#include <sqlite3.h>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr,
            "Usage: %s <repo_path> <db_path> [allowed_root]\n", argv[0]);
        return 1;
    }
    const char* repoPath    = argv[1];
    const char* dbPath      = argv[2];
    const char* allowedRoot = (argc == 4) ? argv[3] : "";

    PythonParser parser;
    auto results = parser.parseDirectory(repoPath, allowedRoot);

    std::fprintf(stdout, "[scan] Parsed %zu files.\n", results.size());

    int rc = DbInserter::insertAll(dbPath, results);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[error] DB insert failed (code %d): %s\n",
                     rc, sqlite3_errstr(rc));
        return 1;
    }

    std::fprintf(stdout, "[scan] Inserted results into %s\n", dbPath);
    return 0;
}
