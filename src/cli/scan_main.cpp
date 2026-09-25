// src/scan_main.cpp
// `Telescode scan` -- parse a repo into a fresh database.
#include "cli/ts_cli.h"
#include "db/DbInserter.h"
#include "parser/ParserRegistry.h"
#include "parser/PythonParser.h"
#include <sqlite3.h>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace TS {

int CmdScan(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr,
            "Usage: Telescode scan <repo_path> <db_path> [allowed_root]\n");
        return 1;
    }
    const char* repoPath    = argv[1];
    const char* dbPath      = argv[2];
    const char* allowedRoot = (argc == 4) ? argv[3] : repoPath;

    ParserRegistry registry;
    registry.registerParser(".py", std::make_unique<PythonParser>());

    auto t0 = std::chrono::steady_clock::now();
    auto results = registry.parseDirectory(repoPath, allowedRoot);
    auto t1 = std::chrono::steady_clock::now();

    std::fprintf(stdout, "[scan] Parsed %zu files.\n", results.size());

    int rc = DbInserter::insertAll(dbPath, results);
    auto t2 = std::chrono::steady_clock::now();
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[error] DB insert failed (code %d): %s\n",
                     rc, sqlite3_errstr(rc));
        return 1;
    }

    std::fprintf(stdout, "[scan] Inserted results into %s\n", dbPath);

    auto ms = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    std::fprintf(stdout, "[scan] Timing: parse %lld ms, db-insert %lld ms\n",
                 (long long)ms(t0, t1), (long long)ms(t1, t2));
    return 0;
}

} // namespace TS
