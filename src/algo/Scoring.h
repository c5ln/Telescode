#pragma once

#include "AlgoConfig.h"
#include "Graph.h"
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
