#include "algo/AlgoDbWriter.h"
#include "algo/AlgoRunner.h"
#include "algo/SeedIo.h"

#include <cstdio>
#include <stdexcept>
#include <vector>

// TelescodePPR <db_path> <seed_csv> <out_csv>
//
// Personalized PageRank over one already-scanned instance DB.
//
//   seed_csv  file_id,weight   -- raw BM25 scores are fine, they are normalized here
//   out_csv   file_id,ppr      -- every project file, ppr descending
//
// exit 0 = success. Any other code means the instance failed and must be
// counted as such (bench/CONTRACT.md §7), never silently skipped.
//
// It has to run while the instance DB still exists: §6 deletes each DB right
// after feature extraction, so there is no later batch pass to fall back on.
int main(int argc, char* argv[])
{
    if (argc != 4) {
        std::fprintf(stderr, "Usage: TelescodePPR <db_path> <seed_csv> <out_csv>\n");
        return 2;
    }
    const char* dbPath  = argv[1];
    const char* seedCsv = argv[2];
    const char* outCsv  = argv[3];

    try {
        SeedLoadResult seed = loadSeedCsv(seedCsv);
        if (!seed.error.empty()) {
            std::fprintf(stderr, "TelescodePPR: %s\n", seed.error.c_str());
            return 3;
        }
        if (seed.seeds.empty()) {
            std::fprintf(stderr, "TelescodePPR: seed csv has no rows: %s\n", seedCsv);
            return 3;
        }

        AlgoConfig cfg = AlgoDbWriter::loadConfig(dbPath);

        int matched = 0;
        std::vector<PprEntry> rows =
            AlgoRunner::personalizedPageRank(dbPath, seed.seeds, cfg, &matched);

        if (rows.empty()) {
            std::fprintf(stderr, "TelescodePPR: no project files in %s\n", dbPath);
            return 4;
        }
        // Zero matches would fall back to the uniform teleport and emit plain
        // PageRank under the `ppr` header -- a third state the harness cannot
        // see, since it only distinguishes "not run" (NaN) from "unreachable"
        // (0.0). Fail instead of mislabelling it.
        if (matched == 0) {
            std::fprintf(stderr,
                "TelescodePPR: none of the %zu seed file_ids exist in %s "
                "(would degrade to plain PageRank)\n", seed.seeds.size(), dbPath);
            return 5;
        }

        std::string err = writePprCsv(outCsv, rows);
        if (!err.empty()) {
            std::fprintf(stderr, "TelescodePPR: %s\n", err.c_str());
            return 6;
        }

        std::fprintf(stdout,
            "TelescodePPR: %d/%zu seeds matched, %zu files written to %s\n",
            matched, seed.seeds.size(), rows.size(), outCsv);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "TelescodePPR: error: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "TelescodePPR: unknown error\n");
        return 1;
    }
}
