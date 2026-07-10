#pragma once

#include "AlgoConfig.h"
#include "Graph.h"
#include <memory>
#include <string>
#include <vector>

// Percentile-rank normalization: position-based, so a single extreme outlier
// doesn't compress the rest of the population toward 0 the way min-max does.
// rank(i) = (# values strictly less than v[i]) / (N - 1); ties share a rank.
// N==0 -> {}; N==1 -> {0.0}; zero-variance input -> all 0.5.
// Exposed (not file-local static like minmax_normalize) so it's unit-testable.
std::vector<double> percentile_rank_normalize(const std::vector<double>& values);

class PageRank {
public:
    static std::vector<double> compute(const Graph& g, const AlgoConfig& cfg);
};

class ScoreCombiner {
public:
    static std::vector<double> combine(const std::vector<double>& pr,
                                       const std::vector<double>& bc,
                                       double alpha,
                                       double beta);
};

// Per-file inputs for ComplexityScorer, already joined with graph in/out-degree.
// (DB query + graph lookup happen in ComplexityScorer::computeAndWrite.)
struct ComplexityFileMetrics {
    std::string file_id;
    int    max_cyclomatic_complexity = 0;
    double avg_cyclomatic_complexity = 0.0;
    int    max_block_depth           = 0;
    double avg_block_depth           = 0.0;
    int    logical_loc               = 0;
    int    inbound                   = 0;
    int    outbound                  = 0;
};

class ComplexityScorer {
public:
    // Pure computation: pre-blends CC/nesting (max/avg), percentile-ranks all
    // five inputs (cc, nesting, logical_loc, inbound, outbound) independently,
    // and combines them by configured weight. No DB access. Weight groups that
    // don't sum to 1.0 (+/- 0.001) are auto-normalized with a stderr warning.
    // Returns one score per file, same order as `inputs`.
    static std::vector<double> compute(const std::vector<ComplexityFileMetrics>& inputs,
                                        const AlgoConfig& cfg);

    // Reads file_id + the five rollup metrics from the `file` table (excluding
    // is_generated=1 rows unless cfg.complexity_include_generated), joins each
    // file_id to `file_graph` for inbound/outbound degree, computes scores via
    // compute(), and writes complexity_score back to `file` in one transaction.
    // Returns SQLITE_OK or the sqlite error code that failed.
    static int computeAndWrite(sqlite3* db, const Graph& file_graph, const AlgoConfig& cfg);
};

// ── Betweenness Centrality ──────────────────────────────────────────────────
// Pass 1(파일)/Pass 2(함수) 별로 그래프 규모에 따라 BC 계산 전략을 분기한다.

enum class PassLevel { File, Function };

struct IBCStrategy {
    virtual std::vector<double> compute(const Graph& g) const = 0;
    virtual ~IBCStrategy() = default;
};

struct ExactBrandesStrategy : IBCStrategy {
    std::vector<double> compute(const Graph& g) const override;
};

struct SamplingBrandesStrategy : IBCStrategy {
    int      k;
    uint64_t seed;
    SamplingBrandesStrategy(int k_, uint64_t seed_) : k(k_), seed(seed_) {}
    std::vector<double> compute(const Graph& g) const override;
};

// enable_p2_bc=false 시 Pass 2 BC를 전부 0으로 처리
struct ZeroBCStrategy : IBCStrategy {
    std::vector<double> compute(const Graph& g) const override;
};

std::unique_ptr<IBCStrategy> make_bc_strategy(int V, const AlgoConfig& cfg, PassLevel level);
