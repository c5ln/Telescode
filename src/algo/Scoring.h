#pragma once

#include "AlgoConfig.h"
#include "Graph.h"
#include <memory>
#include <vector>

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
