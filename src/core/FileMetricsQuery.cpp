// src/core/FileMetricsQuery.cpp

#include "core/FileMetricsQuery.h"

#include <sqlite3.h>

namespace TS {

std::vector<FileMetricsRow> QueryFileMetrics(sqlite3* db)
{
    std::vector<FileMetricsRow> rows;
    if (!db) return rows;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT file_id, file_name, language, raw_loc, logical_loc, is_generated,"
            "       max_cyclomatic_complexity, avg_cyclomatic_complexity,"
            "       max_block_depth, avg_block_depth,"
            "       max_function_loc, avg_function_loc, complexity_score "
            "FROM file ORDER BY file_id;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return rows;   // table absent -- empty result, same as an empty database

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* fid = sqlite3_column_text(stmt, 0);
        if (!fid) continue;

        FileMetricsRow r;
        r.file_id   = reinterpret_cast<const char*>(fid);
        if (const unsigned char* t = sqlite3_column_text(stmt, 1))
            r.file_name = reinterpret_cast<const char*>(t);
        if (const unsigned char* t = sqlite3_column_text(stmt, 2))
            r.language = reinterpret_cast<const char*>(t);
        r.raw_loc                   = sqlite3_column_int   (stmt, 3);
        r.logical_loc               = sqlite3_column_int   (stmt, 4);
        r.is_generated              = sqlite3_column_int   (stmt, 5) != 0;
        r.max_cyclomatic_complexity = sqlite3_column_int   (stmt, 6);
        r.avg_cyclomatic_complexity = sqlite3_column_double(stmt, 7);
        r.max_block_depth           = sqlite3_column_int   (stmt, 8);
        r.avg_block_depth           = sqlite3_column_double(stmt, 9);
        r.max_function_loc          = sqlite3_column_int   (stmt, 10);
        r.avg_function_loc          = sqlite3_column_double(stmt, 11);
        r.complexity_score          = sqlite3_column_double(stmt, 12);
        rows.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return rows;
}

} // namespace TS
