#pragma once

#include "Graph.h"
#include "AlgoConfig.h"
#include <memory>
#include <vector>

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

std::unique_ptr<IBCStrategy> make_bc_strategy(int V, const AlgoConfig& cfg, PassLevel level);
