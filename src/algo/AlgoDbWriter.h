#pragma once

#include "AlgoRunner.h"

struct sqlite3;

// reading_sequence / reading_sequence_config 테이블을 원자적으로 교체한다.
// BEGIN → deleteExisting → insertAll → updateConfig → COMMIT.
// 중간 실패 시 goto rollback → ROLLBACK으로 부분 기록을 남기지 않는다.
class AlgoDbWriter {
public:
    // reading_sequence_config에서 AlgoConfig를 읽는다. 값이 없으면 기본값 사용.
    static AlgoConfig loadConfig(const char* dbPath);

    // 전체 파이프라인 진입점. 독립적인 DB 연결을 열어 트랜잭션을 수행한다.
    static int write(const char* dbPath, const AlgoRunResult& result,
                     const AlgoConfig& cfg);

private:
    static int deleteExisting(sqlite3* db);
    static int insertAll(sqlite3* db, const AlgoRunResult& result);
    static int updateConfig(sqlite3* db, const AlgoConfig& cfg);
};
