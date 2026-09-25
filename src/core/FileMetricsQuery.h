// src/core/FileMetricsQuery.h
// Reads the per-file rollup metrics the parser and the complexity scorer store
// on the `file` table.
//
// Read-only and additive: this does not recompute anything. complexity_score is
// whatever ComplexityScorer last wrote, so a database whose algo pass has not
// run reports 0.0 for it, exactly as the table holds.

#pragma once

#include <string>
#include <vector>

struct sqlite3;

namespace TS {

// One `file` row. Field names and units match the columns one-for-one.
struct FileMetricsRow {
    std::string file_id;
    std::string file_name;
    std::string language;
    int    raw_loc                   = 0;
    int    logical_loc               = 0;
    bool   is_generated              = false;
    int    max_cyclomatic_complexity = 0;
    double avg_cyclomatic_complexity = 0.0;
    int    max_block_depth           = 0;
    double avg_block_depth           = 0.0;
    int    max_function_loc          = 0;
    double avg_function_loc          = 0.0;
    double complexity_score          = 0.0;
};

// Ordered by file_id so the output is stable across runs.
std::vector<FileMetricsRow> QueryFileMetrics(sqlite3* db);

} // namespace TS
