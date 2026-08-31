// src/algo_main.cpp
// `Telescode algo` -- recompute reading_sequence for an existing database.
#include "cli/ts_cli.h"
#include "algo/AlgoDbWriter.h"
#include "algo/AlgoRunner.h"

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
