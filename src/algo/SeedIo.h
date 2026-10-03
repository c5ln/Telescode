#pragma once

#include "AlgoRunner.h"   // PprEntry
#include "Scoring.h"      // SeedEntry

#include <string>
#include <vector>

// seed CSV / ppr CSV 입출력.
//
// bench 하네스와의 확정 인터페이스:
//   seed CSV  헤더 file_id,weight   (instance_id 없음 — DB 하나가 인스턴스 하나)
//   ppr  CSV  헤더 file_id,ppr
//
// CLI(src/ppr_main.cpp)가 얇게 유지되도록, 그리고 파싱을 단위 테스트할 수 있도록
// src/algo/ 안에 둔다.

// 한 CSV 줄을 필드로 쪼갠다. RFC4180식 큰따옴표 인용을 처리한다
// ("" -> 리터럴 큰따옴표). 경로에 쉼표가 들어가는 경우가 드물지만,
// 조용히 잘린 file_id는 조인 실패로만 드러나서 추적이 어렵다.
std::vector<std::string> splitCsvLine(const std::string& line);

struct SeedLoadResult {
    std::vector<SeedEntry> seeds;
    std::string            error;   // 비어 있으면 성공
};

// seed CSV를 읽는다. 열 순서는 헤더로 찾으므로 무관하다.
// 파싱 실패는 조용히 건너뛰지 않고 줄 번호와 함께 error에 담는다
// (bench/CONTRACT.md §7: 실패한 인스턴스를 조용히 건너뛰지 않는다).
SeedLoadResult loadSeedCsv(const std::string& path);

// ppr CSV를 쓴다. 반환값이 비어 있으면 성공, 아니면 오류 메시지.
std::string writePprCsv(const std::string& path, const std::vector<PprEntry>& rows);
