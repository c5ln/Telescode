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

    // Insert and resolve in one transaction: nothing half-built is left behind.
    Resolver::Stats stats;
    int rc = DbInserter::insertAll(dbPath, results, &stats);
    auto t2 = std::chrono::steady_clock::now();
    if (rc == kNotADatabase) {
        std::fprintf(stderr, "[error] %s is not a Telescode database; refusing to overwrite it\n",
                     dbPath);
        return 1;
    }
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[error] DB insert failed (code %d): %s; nothing from this scan"
                     " was kept\n", rc, sqlite3_errstr(rc));
        return 1;
    }

    std::fprintf(stdout, "[scan] Inserted results into %s\n", dbPath);

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
                 (long long)ms(t0, t1), (long long)ms(t1, t2) - stats.ms, stats.ms);
    return 0;
}

} // namespace TS
