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

// One entry of a seed set: a project file and how strongly the query points at
// it. Weights need not sum to 1 -- makeTeleport / compute normalize.
struct SeedEntry {
    std::string file_id;
    double      weight = 0.0;
};

class PageRank {
public:
    // Plain PageRank: uniform 1/N teleport.
    static std::vector<double> compute(const Graph& g, const AlgoConfig& cfg);

    // Personalized PageRank:
    //   PR(v) = (1-d)*p[v] + d*Σ_{u→v} PR(u)*w(u,v)/out_w(u) + d*dangling*p[v]
    // `teleport` is p[], indexed by NodeId. An empty vector means the uniform
    // teleport and reproduces compute(g, cfg) exactly. A vector that does not
    // sum to 1.0 is normalized internally; an all-zero one degrades to uniform.
    // Throws std::invalid_argument on a size mismatch or a negative/NaN entry.
    static std::vector<double> compute(const Graph& g, const AlgoConfig& cfg,
                                       const std::vector<double>& teleport);

    // Converts a (file_id, weight) seed list into a teleport vector over g's
    // NodeIds, normalized to sum 1.0. file_ids absent from the graph and
    // non-positive weights are dropped; if nothing survives the result is empty,
    // which compute() reads as "uniform".
    static std::vector<double> makeTeleport(const Graph& g,
                                            const std::vector<SeedEntry>& seeds);
};

class ScoreCombiner {
public:
    // norm_mode: 0 = min-max, 1 = percentile rank (see AlgoConfig::score_norm_mode).
    static std::vector<double> combine(const std::vector<double>& pr,
                                       const std::vector<double>& bc,
                                       double alpha,
                                       double beta,
                                       int    norm_mode = 1);

    // Three-term form: adds gamma * (1 - complexity), i.e. an "ease" bonus, so
    // that at comparable importance the easier file scores higher and is read
    // first. `complexity` is indexed by NodeId and already percentile-ranked in
    // [0,1] by ComplexityScorer; an empty vector (or gamma == 0) reduces this
    // exactly to the two-term form.
    static std::vector<double> combine(const std::vector<double>& pr,
                                       const std::vector<double>& bc,
                                       const std::vector<double>& complexity,
                                       double alpha,
                                       double beta,
                                       double gamma,
                                       int    norm_mode          = 1,
                                       double complexity_neutral = 0.5);
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
// 나중에 std::thread로 병렬 처리하자. 속도가 많이 빨라질 것으로 추정
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
