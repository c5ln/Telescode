// src/algo_main.cpp
// `Telescode algo` -- recompute reading_sequence for an existing database.
#include "cli/ts_cli.h"
#include "algo/AlgoDbWriter.h"
#include "algo/AlgoRunner.h"
#include "db/db.h"

#include <sqlite3.h>

#include <cstdio>
#include <stdexcept>

namespace TS {

int CmdAlgo(int argc, char* argv[])
{
    if (argc < 2) {
        std::fprintf(stderr, "Usage: Telescode algo <db_path>\n");
        return 1;
    }
    const char* dbPath = argv[1];

    // AlgoRunner opens the database itself and would just come back empty.
    sqlite3* db = nullptr;
    const int rc = initDb(dbPath, &db);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "[algo] error: %s\n", dbOpenError(dbPath, rc).c_str());
        return 1;
    }
    sqlite3_stmt* stmt = nullptr;
    bool hasFiles = true;
    if (sqlite3_prepare_v2(db, "SELECT EXISTS(SELECT 1 FROM file);", -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
        hasFiles = sqlite3_column_int(stmt, 0) != 0;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    // Still a success: there is nothing wrong with the database itself.
    if (!hasFiles) std::fprintf(stderr, "[algo] warning: %s\n", kNoFilesWarning);

    try {
        AlgoConfig cfg = AlgoDbWriter::loadConfig(dbPath);
        AlgoRunResult result = AlgoRunner::run(dbPath, cfg);
        std::fprintf(stdout, "[algo] computed %zu reading sequence entries\n",
                     result.entries.size());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[algo] error: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "[algo] unknown error\n");
        return 1;
    }
}

} // namespace TS
