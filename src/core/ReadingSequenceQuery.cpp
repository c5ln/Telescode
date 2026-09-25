// src/core/ReadingSequenceQuery.cpp

#include "core/ReadingSequenceQuery.h"

#include <sqlite3.h>

namespace TS {
namespace {

// sqlite3_column_text returns null for a NULL column; the rail used to guard
// file_id this way and the behaviour is kept.
const char* textOr(sqlite3_stmt* stmt, int col, const char* fallback)
{
    const unsigned char* t = sqlite3_column_text(stmt, col);
    return t ? reinterpret_cast<const char*>(t) : fallback;
}

std::vector<ReadingSequenceRow> runQuery(sqlite3* db, const char* sql)
{
    std::vector<ReadingSequenceRow> rows;
    if (!db) return rows;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return rows;   // table absent -- caller's empty state

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* eid = sqlite3_column_text(stmt, 0);
        if (!eid) continue;

        ReadingSequenceRow r;
        r.entity_id      = reinterpret_cast<const char*>(eid);
        r.entity_type    = textOr(stmt, 1, "");
        r.file_id        = textOr(stmt, 2, "");
        r.file_rank      = sqlite3_column_int (stmt, 3);
        r.local_rank     = sqlite3_column_int (stmt, 4);
        r.pagerank_score = sqlite3_column_double(stmt, 5);
        r.bc_score       = sqlite3_column_double(stmt, 6);
        r.combined_score = sqlite3_column_double(stmt, 7);
        rows.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return rows;
}

} // anonymous namespace

std::vector<ReadingSequenceRow> QueryFileReadingSequence(sqlite3* db)
{
    // file_rank is NOT NULL exactly when entity_type = 'file' -- enforced by the
    // CHECK constraint in db.cpp -- so this ordering is total.
    return runQuery(db,
        "SELECT entity_id, entity_type, file_id, file_rank, local_rank,"
        "       pagerank_score, bc_score, combined_score "
        "FROM reading_sequence WHERE entity_type = 'file' "
        "ORDER BY file_rank;");
}

std::vector<ReadingSequenceRow> QueryReadingSequence(sqlite3* db)
{
    // Files ahead of their members, then a total order within each file: the
    // rank columns alone do not order the whole table, since file_rank is NULL
    // on member rows and local_rank is NULL on file rows.
    return runQuery(db,
        "SELECT entity_id, entity_type, file_id, file_rank, local_rank,"
        "       pagerank_score, bc_score, combined_score "
        "FROM reading_sequence "
        "ORDER BY (entity_type = 'file') DESC, file_rank, file_id, local_rank, entity_id;");
}

std::string FileNameOf(const std::string& file_id)
{
    const auto slash = file_id.rfind('/');
    return (slash == std::string::npos) ? file_id : file_id.substr(slash + 1);
}

std::string DirNameOf(const std::string& file_id)
{
    const auto slash = file_id.rfind('/');
    return (slash == std::string::npos) ? std::string() : file_id.substr(0, slash);
}

} // namespace TS
