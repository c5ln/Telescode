#include "algo/AlgoDbWriter.h"
#include "algo/AlgoRunner.h"

#include <cstdio>
#include <stdexcept>

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::fprintf(stderr, "Usage: TelescodeAlgo <db_path>\n");
        return 1;
    }
    const char* dbPath = argv[1];

    try {
        AlgoConfig cfg = AlgoDbWriter::loadConfig(dbPath);
        AlgoRunResult result = AlgoRunner::run(dbPath, cfg);
        std::fprintf(stdout, "TelescodeAlgo: computed %zu reading sequence entries\n",
                     result.entries.size());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "TelescodeAlgo: error: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "TelescodeAlgo: unknown error\n");
        return 1;
    }
}
