// src/core/ReadingSequenceQuery.h
// Reads the reading_sequence table the algo pass writes.
//
// The query used to live inside ts_rail.cpp, which meant the only way to get at
// the reading order was to draw it. Nothing about reading those rows needs a
// font atlas, so it sits here instead and the rail consumes the result.

#pragma once

#include <string>
#include <vector>

struct sqlite3;

namespace TS {

// One reading_sequence row. Mirrors the table: file_rank is set exactly when
// entity_type == "file", local_rank exactly when it is not -- the CHECK
// constraint in db.cpp enforces that, so only one of the pair is meaningful.
struct ReadingSequenceRow {
    std::string entity_id;
    std::string entity_type;     // "file" | "class" | "function"
    std::string file_id;
    int         file_rank      = 0;
    int         local_rank     = 0;
    double      pagerank_score = 0.0;
    double      bc_score       = 0.0;
    double      combined_score = 0.0;
};

// Files only, in reading order. Empty when db is null or the table is absent
// (a database from before the algo ran) -- callers show an empty state.
std::vector<ReadingSequenceRow> QueryFileReadingSequence(sqlite3* db);

// Every row: files first in file_rank order, then the per-file entities ordered
// by file_id and local_rank, so the result is stable across runs.
std::vector<ReadingSequenceRow> QueryReadingSequence(sqlite3* db);

// "src/ui/ts_app.cpp" -> "ts_app.cpp" / "src/ui". A bare filename has no
// directory, and its name is the whole string.
std::string FileNameOf(const std::string& file_id);
std::string DirNameOf (const std::string& file_id);

} // namespace TS
