#include "AlgoDbWriter.h"
#include "db/db.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sqlite3.h>

// ── config helpers ────────────────────────────────────────────────────────────

static double get_double(sqlite3* db, const char* key, double def)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT config_value FROM reading_sequence_config WHERE config_key = ?;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return def;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    double val = def;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (v) val = std::atof(v);
    }
    sqlite3_finalize(stmt);
    return val;
}

static int get_int(sqlite3* db, const char* key, int def)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT config_value FROM reading_sequence_config WHERE config_key = ?;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return def;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    int val = def;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (v) val = std::atoi(v);
    }
    sqlite3_finalize(stmt);
    return val;
}

static uint64_t get_uint64(sqlite3* db, const char* key, uint64_t def)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT config_value FROM reading_sequence_config WHERE config_key = ?;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return def;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    uint64_t val = def;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (v) val = static_cast<uint64_t>(std::strtoull(v, nullptr, 10));
    }
    sqlite3_finalize(stmt);
    return val;
}

static void upsert(sqlite3* db, const char* key, const char* value)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT INTO reading_sequence_config(config_key, config_value) VALUES(?,?)"
        " ON CONFLICT(config_key) DO UPDATE SET config_value=excluded.config_value;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, key,   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// ── AlgoDbWriter::loadConfig ─────────────────────────────────────────────────

AlgoConfig AlgoDbWriter::loadConfig(const char* dbPath)
{
    sqlite3* db = nullptr;
    AlgoConfig cfg;
    if (initDb(dbPath, &db) != SQLITE_OK) return cfg;

    cfg.alpha           = get_double(db, "alpha",         cfg.alpha);
    cfg.beta            = get_double(db, "beta",          cfg.beta);
    cfg.damping         = get_double(db, "damping",       cfg.damping);
    cfg.max_iter        = get_int   (db, "max_iter",      cfg.max_iter);
    cfg.convergence_eps = get_double(db, "eps",           cfg.convergence_eps);
    cfg.bc_p1_exact_v   = get_int   (db, "bc_p1_exact_v", cfg.bc_p1_exact_v);
    cfg.bc_p1_large_v   = get_int   (db, "bc_p1_large_v", cfg.bc_p1_large_v);
    cfg.bc_p1_fixed_k   = get_int   (db, "bc_p1_fixed_k", cfg.bc_p1_fixed_k);
    cfg.enable_p2_bc    = get_int   (db, "enable_p2_bc",  cfg.enable_p2_bc ? 1 : 0) != 0;
    cfg.bc_p2_exact_v   = get_int   (db, "bc_p2_exact_v", cfg.bc_p2_exact_v);
    cfg.bc_p2_large_v   = get_int   (db, "bc_p2_large_v", cfg.bc_p2_large_v);
    cfg.bc_p2_fixed_k   = get_int   (db, "bc_p2_fixed_k", cfg.bc_p2_fixed_k);
    cfg.bc_k_min        = get_int   (db, "bc_k_min",      cfg.bc_k_min);
    cfg.bc_seed         = get_uint64(db, "bc_seed",       cfg.bc_seed);

    sqlite3_close(db);
    return cfg;
}

// ── AlgoDbWriter::deleteExisting ─────────────────────────────────────────────

int AlgoDbWriter::deleteExisting(sqlite3* db)
{
    char* err = nullptr;
    int rc = sqlite3_exec(db,
        "DELETE FROM reading_sequence;"
        "DELETE FROM reading_sequence_config;",
        nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "AlgoDbWriter: delete failed: %s\n", err ? err : "");
        sqlite3_free(err);
    }
    return rc;
}

// ── AlgoDbWriter::insertAll ──────────────────────────────────────────────────

int AlgoDbWriter::insertAll(sqlite3* db, const AlgoRunResult& result)
{
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO reading_sequence"
        "(entity_id, entity_type, file_id, file_rank, local_rank,"
        " pagerank_score, bc_score, combined_score)"
        " VALUES (?,?,?,?,?,?,?,?);",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return rc;

    for (const ReadingEntry& e : result.entries) {
        sqlite3_bind_text(stmt, 1, e.entity_id.c_str(),   -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, e.entity_type.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, e.file_id.c_str(),     -1, SQLITE_STATIC);

        // DB CHECK 제약: file일 때 file_rank NOT NULL / local_rank NULL, 그 반대도 동일
        if (e.entity_type == "file")
            sqlite3_bind_int(stmt, 4, e.file_rank);
        else
            sqlite3_bind_null(stmt, 4);

        if (e.entity_type != "file")
            sqlite3_bind_int(stmt, 5, e.local_rank);
        else
            sqlite3_bind_null(stmt, 5);

        sqlite3_bind_double(stmt, 6, e.pagerank_score);
        sqlite3_bind_double(stmt, 7, e.bc_score);
        sqlite3_bind_double(stmt, 8, e.combined_score);

        rc = sqlite3_step(stmt);
        sqlite3_reset(stmt);
        if (rc != SQLITE_DONE) { sqlite3_finalize(stmt); return rc; }
    }

    sqlite3_finalize(stmt);
    return SQLITE_OK;
}

// ── AlgoDbWriter::updateConfig ───────────────────────────────────────────────

int AlgoDbWriter::updateConfig(sqlite3* db, const AlgoConfig& cfg)
{
    char buf[64];

    auto d = [&](const char* k, double v) {
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        upsert(db, k, buf);
    };
    auto i = [&](const char* k, int v) {
        std::snprintf(buf, sizeof(buf), "%d", v);
        upsert(db, k, buf);
    };
    auto u = [&](const char* k, uint64_t v) {
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
        upsert(db, k, buf);
    };

    d("alpha",         cfg.alpha);
    d("beta",          cfg.beta);
    d("damping",       cfg.damping);
    i("max_iter",      cfg.max_iter);
    d("eps",           cfg.convergence_eps);
    i("bc_p1_exact_v", cfg.bc_p1_exact_v);
    i("bc_p1_large_v", cfg.bc_p1_large_v);
    i("bc_p1_fixed_k", cfg.bc_p1_fixed_k);
    i("enable_p2_bc",  cfg.enable_p2_bc ? 1 : 0);
    i("bc_p2_exact_v", cfg.bc_p2_exact_v);
    i("bc_p2_large_v", cfg.bc_p2_large_v);
    i("bc_p2_fixed_k", cfg.bc_p2_fixed_k);
    i("bc_k_min",      cfg.bc_k_min);
    u("bc_seed",       cfg.bc_seed);

    char ts[32];
    std::time_t now = std::time(nullptr);
    std::snprintf(ts, sizeof(ts), "%lld", static_cast<long long>(now));
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT INTO reading_sequence_config(config_key, config_value) VALUES('last_computed_at',?)"
        " ON CONFLICT(config_key) DO UPDATE SET config_value=excluded.config_value;",
        -1, &stmt, nullptr);
    if (stmt) {
        sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    return SQLITE_OK;
}

// ── AlgoDbWriter::write ───────────────────────────────────────────────────────

int AlgoDbWriter::write(const char* dbPath, const AlgoRunResult& result,
                         const AlgoConfig& cfg)
{
    sqlite3* db = nullptr;
    int rc = initDb(dbPath, &db);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) { sqlite3_close(db); return rc; }

    rc = deleteExisting(db);
    if (rc != SQLITE_OK) goto rollback;

    rc = insertAll(db, result);
    if (rc != SQLITE_OK) goto rollback;

    rc = updateConfig(db, cfg);
    if (rc != SQLITE_OK) goto rollback;

    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return SQLITE_OK;

rollback:
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return rc;
}
