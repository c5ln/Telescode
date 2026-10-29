// src/scan_main.cpp
// `Telescode scan` -- parse a repo into a fresh database.
#include "cli/ts_cli.h"
#include "db/DbInserter.h"
#include "db/db.h"
#include "parser/ParserRegistry.h"
#include "parser/PythonParser.h"
#include "resolve/Resolver.h"
#include <sqlite3.h>
#include <chrono>
#include <cstdio>
#include <map>
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

    // Names across files can only be resolved once every file is in.
    Resolver::Stats stats;
    sqlite3* db = nullptr;
    rc = initDb(dbPath, &db);
    if (rc == SQLITE_OK) rc = Resolver::run(db, &stats);
    sqlite3_close(db);
    auto t3 = std::chrono::steady_clock::now();
    if (rc != SQLITE_OK) {
        // insertAll has just stamped the database, so kNeedsRescan means the
        // stamp did not stick; any other code is SQLite's.
        std::fprintf(stderr, "[error] name resolution failed (code %d): %s\n", rc,
                     rc == Resolver::kNeedsRescan ? "the database has no current scan stamp"
                                                  : sqlite3_errstr(rc));
        return 1;
    }

    auto count = [](const std::map<std::string, int>& m, const char* k) {
        auto it = m.find(k);
        return it == m.end() ? 0 : it->second;
    };
    std::fprintf(stdout,
                 "[scan] Resolved refs: %d resolved, %d builtin, %d external, %d unresolved;"
                 " links: %d CALLS, %d INHERITS, %d DECORATES\n",
                 count(stats.status, "resolved"), count(stats.status, "builtin"),
                 count(stats.status, "external"), count(stats.status, "unresolved"),
                 count(stats.links, "CALLS"), count(stats.links, "INHERITS"),
                 count(stats.links, "DECORATES"));
    Resolver::warnIfCapped(stats, "scan");

    auto ms = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    std::fprintf(stdout, "[scan] Timing: parse %lld ms, db-insert %lld ms, resolve %lld ms\n",
                 (long long)ms(t0, t1), (long long)ms(t1, t2), (long long)ms(t2, t3));
    return 0;
}

} // namespace TS
