#pragma once

#include "AlgoConfig.h"
#include "Graph.h"
#include "GraphBuilder.h"           // GraphBuilderResult
#include "Scoring.h"                // PassLevel
#include <string>
#include <vector>

// reading_sequence 테이블 한 행에 대응하는 엔티티 읽기 정보
struct ReadingEntry {
    std::string entity_id;             // 엔티티 고유 ID (file_id, function_id, class_id)
    std::string entity_type;           // "file" | "class" | "function"
    std::string file_id;               // 소속 파일 (entity_type=="file"이면 entity_id와 동일)
    int         file_rank      = 0;    // 파일 간 읽기 순서 (1-indexed, entity_type=="file"일 때만 유효)
    int         local_rank     = 0;    // 파일 내 읽기 순서 (1-indexed, entity_type!="file"일 때만 유효)
    double      pagerank_score = 0.0;
    double      bc_score       = 0.0;
    double      combined_score = 0.0;
};

// AlgoRunner::run()의 반환값; reading_sequence 테이블 전체 내용을 메모리에 보관
struct AlgoRunResult {
    std::vector<ReadingEntry> entries;
};

// 단일 패스(Pass 1 또는 Pass 2)의 중간 계산 결과
struct AlgoPassResult {
    std::vector<double> pr;   // 노드별 PageRank 점수 (raw, NodeId 인덱스)
    std::vector<double> bc;   // 노드별 BC 점수 (raw, NodeId 인덱스)
    std::vector<double> sc;   // 노드별 결합 점수 (min-max 정규화 후 가중합)
    std::vector<NodeId> seq;  // 읽기 순서로 정렬된 NodeId 배열 (index 0 = 첫 번째로 읽을 노드)
};

// Template Method 패턴: PageRank → BC → ScoreCombiner → ReadingSequencer 공통 흐름.
// FilePass(Pass 1)와 FunctionPass(Pass 2)가 level()만 오버라이드해 BC 전략을 분기한다.
class AlgoPass {
public:
    // loc_hint: 동점 시 LOC 내림차순 보조 정렬에 사용 (NodeId 인덱스, 없으면 빈 벡터)
    AlgoPassResult run(const Graph& g, const AlgoConfig& cfg,
                       const std::vector<int>& loc_hint = {});
protected:
    virtual PassLevel level() const = 0;
    virtual ~AlgoPass() = default;
};

// Pass 1: 파일 레벨 그래프에 대해 알고리즘을 실행한다
class FilePass : public AlgoPass {
protected:
    PassLevel level() const override { return PassLevel::File; }
};

// Pass 2: 함수/클래스 레벨 그래프에 대해 알고리즘을 실행한다
class FunctionPass : public AlgoPass {
protected:
    PassLevel level() const override { return PassLevel::Function; }
};

struct sqlite3;

// 2-pass 파이프라인 조율자.
// GraphBuilder → FilePass → FunctionPass → merge → AlgoDbWriter 순으로 실행한다.
class AlgoRunner {
public:
    // DB를 읽어 reading_sequence를 계산하고 결과를 DB에 쓴 뒤 반환한다.
    static AlgoRunResult run(const char* dbPath, const AlgoConfig& cfg);

    // Pass 1(파일) 결과와 Pass 2(함수/클래스) 결과를 ReadingEntry 목록으로 합친다.
    // - 파일 엔티티: file_rank = Pass 1 순서 (1-indexed)
    // - 함수/클래스: 파일별 그룹화 후 global_rank → local_rank 변환 (1-indexed)
    // - 동점 보조 정렬: start_line ASC
    static AlgoRunResult merge(const AlgoPassResult&    file_result,
                                const Graph&             file_graph,
                                const AlgoPassResult&    func_result,
                                const Graph&             func_graph,
                                const GraphBuilderResult& gbr);
};
