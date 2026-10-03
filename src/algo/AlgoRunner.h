#pragma once

#include "AlgoConfig.h"
#include "Graph.h"                  // Graph, GraphBuilderResult
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

// Personalized PageRank 결과 한 행. bench 피처 매트릭스의 `ppr` 컬럼 원본
// (bench/schema.py C.ppr). reading_sequence에는 아직 저장 경로가 없다.
struct PprEntry {
    std::string file_id;
    double      ppr = 0.0;
};

// AlgoRunner::run()의 반환값; reading_sequence 테이블 전체 내용을 메모리에 보관
struct AlgoRunResult {
    std::vector<ReadingEntry> entries;
};

// 단일 패스(Pass 1 또는 Pass 2)의 중간 계산 결과
struct AlgoPassResult {
    std::vector<double> pr;   // 노드별 PageRank 점수 (raw, NodeId 인덱스)
    std::vector<double> bc;   // 노드별 BC 점수 (raw, NodeId 인덱스)
    std::vector<double> sc;   // 노드별 결합 점수 (PR/BC 정규화 + ease 항 가중합)
    std::vector<NodeId> seq;  // 읽기 순서로 정렬된 NodeId 배열 (index 0 = 첫 번째로 읽을 노드)
};

// Template Method 패턴: PageRank → BC → ScoreCombiner → ReadingSequencer 공통 흐름.
// FilePass(Pass 1)와 FunctionPass(Pass 2)가 level()만 오버라이드해 BC 전략을 분기한다.
class AlgoPass {
public:
    // complexity: 노드별 이해 비용 [0,1] (NodeId 인덱스). combined_score의 gamma
    // 항(= 1 - complexity)과 동점 시 오름차순 보조 정렬에 함께 쓰인다.
    // 빈 벡터면 두 경로 모두 비활성 → 기존 PR/BC 전용 동작과 동일.
    AlgoPassResult run(const Graph& g, const AlgoConfig& cfg,
                       const std::vector<double>& complexity = {});
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

    // file.complexity_score를 file_graph의 NodeId 인덱스 벡터로 읽어온다.
    // `file` 행이 없거나 ComplexityScorer가 건너뛴 노드는 cfg.complexity_neutral.
    static std::vector<double> loadFileComplexity(sqlite3*          db,
                                                   const Graph&      file_graph,
                                                   const AlgoConfig& cfg);

    // seed에서 출발하는 Personalized PageRank를 파일 그래프 위에서 계산한다.
    // seeds가 비었거나 어느 file_id도 그래프에 없으면 균등 teleport로 폴백해
    // 기존 PageRank와 동일한 결과를 낸다.
    // 반환: 실제 `file` 행이 있는 노드만, ppr 내림차순. 해소되지 않은 import
    // 대상(외부 모듈)은 그래프에는 있지만 결과에서 제외된다.
    // matched_seeds(선택): 실제로 그래프 노드에 대응된 seed 파일 수. 0이면 결과가
    // PPR이 아니라 plain PageRank다 — 호출자가 그 둘을 구분해야 할 때 쓴다.
    static std::vector<PprEntry> personalizedPageRank(const char* dbPath,
                                                       const std::vector<SeedEntry>& seeds,
                                                       const AlgoConfig& cfg,
                                                       int* matched_seeds = nullptr);
};
